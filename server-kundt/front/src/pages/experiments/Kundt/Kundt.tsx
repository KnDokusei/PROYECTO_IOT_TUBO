import { Alert, Box, Card, CardContent, Chip, Grid, LinearProgress, Stack, TextField, Typography, useTheme } from "@mui/material";
import { useEffect, useRef, useState } from "react";
import { useLoaderData, useParams } from "react-router";
import { ResponsiveLine } from "@nivo/line";

import CamerasDisplay from "../../../components/cameraviewer/CamerasDisplay";
import SliderComponent from "../../../components/slidercomponent/SliderComponent";
import { ButtonCB } from "../../../components/buttons/button";
import { CardTitle } from "../../../components/misc/Title";
import { useSnackbar } from "../../../components/toast/ToastProvider";
import UserGrid from "../../../components/user-grid/UserGrid";
import generatePlotTheme from "../PlotTheme";
import { CONTROL_BORDER_CARD_RAIDUS } from "../Constants";

import { getCameraList } from "../../../api/camera";
import { updateKundt } from "../../../api/experiments/kundt";
import { startAuthenticatedSSE } from "../../../api/experiments/sse";
import { SSE_KUNDT_UPDATES } from "../../../api/routes";

import type { CameraOBJ } from "../../../types/cameras";
import type { KundtSample, KundtSensors, KundtSSE, KundtStatusDTO } from "../../../types/experiments/kundt";

import GraphicEqIcon from "@mui/icons-material/GraphicEq";
import VolumeUpIcon from "@mui/icons-material/VolumeUp";
import StraightenIcon from "@mui/icons-material/Straighten";
import HomeIcon from "@mui/icons-material/Home";
import PlayArrowIcon from "@mui/icons-material/PlayArrow";
import StopIcon from "@mui/icons-material/Stop";
import DeleteSweepIcon from "@mui/icons-material/DeleteSweep";

/* Firmware limits, mirrored here so the UI rejects before the round trip. */
const FREQ_MIN_HZ = 20;
const FREQ_MAX_HZ = 20000;
const SERVO_MAX_DEG = 180;

/* mic_peak is a 16-bit magnitude. Past this the analog front end is clipping
 * and the amplitude reading stops being trustworthy. */
const PEAK_FULL_SCALE = 32767;
const PEAK_SATURATION = 31000;

/* Below this ratio the microphone is hearing room noise, not the tone. */
const TONE_RATIO_THRESHOLD = 50;

/* Speed of sound at 20 C, only used to label the expected spacing. */
const REFERENCE_SPEED_MS = 343;

const MAX_SAMPLES = 600;

const VOLUME_MARKS = [
    { value: 0, label: "0" },
    { value: 45, label: "45" },
    { value: 90, label: "90" },
    { value: 135, label: "135" },
    { value: 180, label: "180" },
];

/*
 * Resonance finder.
 *
 * Consecutive maxima of the standing wave sit half a wavelength apart, so the
 * mean spacing between peaks gives v = 2 * spacing * frequency. Peaks are taken
 * as samples above 60% of the sweep's range that also dominate their
 * neighbourhood, which is enough for a hand-driven sweep and avoids reporting
 * every ripple as a resonance.
 */
function findResonances(samples: KundtSample[]): number[] {
    if (samples.length < 12) return [];

    const sorted = [...samples].sort((a, b) => a.position - b.position);
    const amplitudes = sorted.map((s) => s.amplitude);
    const lo = Math.min(...amplitudes);
    const hi = Math.max(...amplitudes);
    if (hi <= lo) return [];

    const threshold = lo + (hi - lo) * 0.6;
    const window = 3;
    const peaks: number[] = [];

    for (let i = window; i < sorted.length - window; i++) {
        const here = sorted[i].amplitude;
        if (here < threshold) continue;

        let dominates = true;
        for (let k = i - window; k <= i + window; k++) {
            if (k !== i && sorted[k].amplitude > here) { dominates = false; break; }
        }
        if (!dominates) continue;

        /* Collapse peaks closer than 2 cm: the same maximum sampled twice. */
        const position = sorted[i].position;
        if (peaks.length > 0 && Math.abs(position - peaks[peaks.length - 1]) < 2) continue;
        peaks.push(position);
    }

    return peaks;
}

function speedFromResonances(peaks: number[], frequency: number): number | null {
    if (peaks.length < 2 || frequency <= 0) return null;

    let total = 0;
    for (let i = 1; i < peaks.length; i++) total += peaks[i] - peaks[i - 1];
    const mean_spacing_cm = total / (peaks.length - 1);

    /* spacing is half a wavelength; cm -> m */
    return 2 * (mean_spacing_cm / 100) * frequency;
}

export default function Kundt() {
    const initial = useLoaderData() as KundtStatusDTO;
    const { id } = useParams();
    const platform_id = Number(id);
    const theme = useTheme();
    const { showSnackBar } = useSnackbar();

    const [cameras, setCameras] = useState<CameraOBJ[]>([]);
    const [sensors, setSensors] = useState<KundtSensors>({
        mic_amplitude: initial.mic_amplitude,
        mic_rms: initial.mic_rms,
        mic_peak: initial.mic_peak,
        plunger_actual: initial.plunger_actual,
        clockwise_limit: initial.clockwise_limit,
        counter_limit: initial.counter_limit,
    });

    const [frequency, setFrequency] = useState<number>(initial.frequency);
    const [frequencyInput, setFrequencyInput] = useState<string>(String(initial.frequency));
    const [volume, setVolume] = useState<number>(initial.volume);
    const [plungerTarget, setPlungerTarget] = useState<string>(String(initial.plunger_pos));

    const [samples, setSamples] = useState<KundtSample[]>([]);
    const [listening, setListening] = useState<boolean>(false);

    /* Web Audio graph for the reconstructed tone. Kept in refs so re-renders do
     * not rebuild it and click on every update. */
    const audioContextRef = useRef<AudioContext | null>(null);
    const oscillatorRef = useRef<OscillatorNode | null>(null);
    const gainRef = useRef<GainNode | null>(null);

    /* Latest sensor values, read by the audio updater without re-subscribing. */
    const sensorsRef = useRef<KundtSensors>(sensors);
    sensorsRef.current = sensors;

    const amplitude = sensors.mic_amplitude ?? 0;
    const rms = sensors.mic_rms ?? 0;
    const peak = sensors.mic_peak ?? 0;
    const toneRatio = rms > 0 ? (amplitude / rms) * 100 : 0;
    const saturating = peak >= PEAK_SATURATION;
    const headroom = peak > 0 ? PEAK_FULL_SCALE / peak : 0;

    const resonances = findResonances(samples);
    const measuredSpeed = speedFromResonances(resonances, frequency);
    const expectedSpacing = frequency > 0 ? (REFERENCE_SPEED_MS / frequency / 2) * 100 : 0;

    useEffect(() => {
        getCameraList(platform_id)
            .then(setCameras)
            .catch(() => showSnackBar({ severity: "error", message: "No se pudieron cargar las cámaras." }));
    }, [platform_id]);

    useEffect(() => {
        const closeSSE = startAuthenticatedSSE<KundtSSE>({
            url: SSE_KUNDT_UPDATES,
            onMessage: (data) => {
                if (data.sensors) {
                    setSensors((previous) => ({ ...previous, ...data.sensors }));

                    /* Only log a sweep point when both halves are present: the
                     * measurement is meaningless without the position it was
                     * taken at, and E1 and E3 publish independently. */
                    const position = data.sensors.plunger_actual;
                    const level = data.sensors.mic_amplitude;
                    if (position !== undefined && level !== undefined) {
                        setSamples((previous) => {
                            const next = [...previous, { position, amplitude: level }];
                            return next.length > MAX_SAMPLES ? next.slice(next.length - MAX_SAMPLES) : next;
                        });
                    }
                }
                if (data.actuators) {
                    if (data.actuators.frequency !== undefined) setFrequency(data.actuators.frequency);
                    if (data.actuators.volume !== undefined) setVolume(data.actuators.volume);
                }
            },
            onError: () => showSnackBar({ severity: "error", message: "Se perdió la conexión con el experimento." }),
        });
        return closeSSE;
    }, []);

    /* Reconstructed audio. The microphone sends scalars, never waveforms, so
     * this is a synthesised tone at the driven frequency whose gain follows the
     * Goertzel magnitude. It shows the resonance; it is not what the mic heard. */
    useEffect(() => {
        if (!listening) return;

        const context = new AudioContext();
        const oscillator = context.createOscillator();
        const gain = context.createGain();

        oscillator.type = "sine";
        oscillator.frequency.setValueAtTime(frequency, context.currentTime);
        gain.gain.setValueAtTime(0, context.currentTime);
        oscillator.connect(gain);
        gain.connect(context.destination);
        oscillator.start();

        audioContextRef.current = context;
        oscillatorRef.current = oscillator;
        gainRef.current = gain;

        /* Full scale would be painfully loud; this keeps a resonance peak near
         * a comfortable level while silence stays silent. */
        const interval = window.setInterval(() => {
            const current = sensorsRef.current;
            const level = current.mic_amplitude ?? 0;
            const reference = current.mic_rms && current.mic_rms > 0 ? current.mic_rms : 1;
            const normalised = Math.min(1, level / Math.max(reference, 1));
            gain.gain.linearRampToValueAtTime(normalised * 0.25, context.currentTime + 0.15);
        }, 120);

        return () => {
            window.clearInterval(interval);
            try { oscillator.stop(); } catch { /* already stopped */ }
            oscillator.disconnect();
            gain.disconnect();
            context.close();
            audioContextRef.current = null;
            oscillatorRef.current = null;
            gainRef.current = null;
        };
    }, [listening]);

    useEffect(() => {
        const context = audioContextRef.current;
        const oscillator = oscillatorRef.current;
        if (context && oscillator) {
            oscillator.frequency.linearRampToValueAtTime(frequency, context.currentTime + 0.05);
        }
    }, [frequency]);

    const send = async (body: Parameters<typeof updateKundt>[1], message?: string) => {
        try {
            await updateKundt(platform_id, body);
            if (message) showSnackBar({ severity: "success", message });
        }
        catch {
            showSnackBar({ severity: "error", message: "No se pudo enviar la instrucción al experimento." });
        }
    };

    const commitFrequency = () => {
        const parsed = Number(frequencyInput);
        if (!Number.isFinite(parsed)) {
            showSnackBar({ severity: "warning", message: "La frecuencia debe ser un número." });
            return;
        }
        if (parsed < FREQ_MIN_HZ || parsed > FREQ_MAX_HZ) {
            showSnackBar({
                severity: "warning",
                message: `La frecuencia debe estar entre ${FREQ_MIN_HZ} y ${FREQ_MAX_HZ} Hz.`,
            });
            return;
        }
        setFrequency(parsed);
        send({ frequency: parsed });
    };

    const commitVolume = (value: number) => {
        setVolume(value);
        send({ volume: value });
    };

    const commitPlunger = () => {
        const parsed = Number(plungerTarget);
        if (!Number.isFinite(parsed)) {
            showSnackBar({ severity: "warning", message: "La posición debe ser un número." });
            return;
        }
        send({ plunger_pos: parsed }, `Émbolo enviado a ${parsed} cm.`);
    };

    const calibrate = () => {
        send({ calibrate: true }, "Calibración solicitada: el émbolo buscará el fin de carrera.");
    };

    const chartData = [{
        id: "Amplitud",
        data: [...samples]
            .sort((a, b) => a.position - b.position)
            .map((s) => ({ x: s.position, y: Math.round(s.amplitude) })),
    }];

    const resonanceRows = resonances.map((position, index) => ({
        id: index,
        orden: index + 1,
        posicion: position.toFixed(1),
        separacion: index === 0 ? "—" : (position - resonances[index - 1]).toFixed(1),
    }));

    return (
        <Stack spacing={2.5} sx={{ padding: 2 }}>

            <CamerasDisplay camera_list={cameras} />

            <Grid container spacing={2.5}>

                {/* ---------------- Generador de audio (E2) ---------------- */}
                <Grid size={{ xs: 12, md: 6 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS, height: "100%" }}>
                        <CardContent>
                            <CardTitle label="Generador de audio" icon={<GraphicEqIcon />} />

                            <Stack spacing={2} sx={{ marginTop: 2 }}>
                                <Stack direction="row" spacing={1} alignItems="center">
                                    <TextField
                                        label="Frecuencia [Hz]"
                                        value={frequencyInput}
                                        onChange={(e) => setFrequencyInput(e.target.value)}
                                        onKeyDown={(e) => { if (e.key === "Enter") commitFrequency(); }}
                                        size="small"
                                        type="number"
                                        inputProps={{ min: FREQ_MIN_HZ, max: FREQ_MAX_HZ }}
                                    />
                                    <ButtonCB variant="contained" onClick={commitFrequency}>Aplicar</ButtonCB>
                                </Stack>

                                <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
                                    {[350, 500, 1000, 1500, 2000].map((preset) => (
                                        <Chip
                                            key={preset}
                                            label={`${preset} Hz`}
                                            onClick={() => { setFrequencyInput(String(preset)); setFrequency(preset); send({ frequency: preset }); }}
                                            color={frequency === preset ? "primary" : "default"}
                                            variant={frequency === preset ? "filled" : "outlined"}
                                        />
                                    ))}
                                </Stack>

                                <Box>
                                    <Typography variant="body2" sx={{ marginBottom: 1 }}>
                                        Volumen: <b>{volume}°</b> de {SERVO_MAX_DEG}°
                                    </Typography>
                                    <SliderComponent
                                        value={volume}
                                        setValue={commitVolume}
                                        Icon={VolumeUpIcon}
                                        Marks={VOLUME_MARKS}
                                    />
                                    <Typography variant="caption" color="text.secondary">
                                        Es el ángulo del servo que gira el potenciómetro, no un porcentaje.
                                    </Typography>
                                </Box>
                            </Stack>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Émbolo (E3) ---------------- */}
                <Grid size={{ xs: 12, md: 6 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS, height: "100%" }}>
                        <CardContent>
                            <CardTitle label="Émbolo" icon={<StraightenIcon />} />

                            <Stack spacing={2} sx={{ marginTop: 2 }}>
                                <Typography variant="h5">
                                    {sensors.plunger_actual !== undefined ? `${sensors.plunger_actual.toFixed(2)} cm` : "sin dato"}
                                </Typography>

                                <Stack direction="row" spacing={1} alignItems="center">
                                    <TextField
                                        label="Ir a [cm]"
                                        value={plungerTarget}
                                        onChange={(e) => setPlungerTarget(e.target.value)}
                                        onKeyDown={(e) => { if (e.key === "Enter") commitPlunger(); }}
                                        size="small"
                                        type="number"
                                    />
                                    <ButtonCB variant="contained" onClick={commitPlunger}>Mover</ButtonCB>
                                </Stack>

                                <Stack direction="row" spacing={1}>
                                    <Chip
                                        label={sensors.clockwise_limit ? "Fin de carrera +" : "Carrera + libre"}
                                        color={sensors.clockwise_limit ? "warning" : "default"}
                                        size="small"
                                    />
                                    <Chip
                                        label={sensors.counter_limit ? "Fin de carrera −" : "Carrera − libre"}
                                        color={sensors.counter_limit ? "warning" : "default"}
                                        size="small"
                                    />
                                </Stack>

                                <ButtonCB variant="outlined" startIcon={<HomeIcon />} onClick={calibrate}>
                                    Calibrar
                                </ButtonCB>
                                <Typography variant="caption" color="text.secondary">
                                    Lleva el émbolo contra el fin de carrera y fija el cero. Hazlo antes de medir.
                                </Typography>
                            </Stack>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Micrófono (E1) ---------------- */}
                <Grid size={{ xs: 12, md: 6 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS, height: "100%" }}>
                        <CardContent>
                            <CardTitle label="Micrófono" icon={<GraphicEqIcon />} />

                            <Stack spacing={2} sx={{ marginTop: 2 }}>
                                <Typography variant="h4">{Math.round(amplitude)}</Typography>
                                <Typography variant="caption" color="text.secondary">
                                    Amplitud a {frequency} Hz, medida por Goertzel
                                </Typography>

                                <Box>
                                    <Typography variant="body2">
                                        Señal útil: <b>{toneRatio.toFixed(1)} %</b>
                                    </Typography>
                                    <LinearProgress
                                        variant="determinate"
                                        value={Math.min(100, toneRatio)}
                                        color={toneRatio >= TONE_RATIO_THRESHOLD ? "success" : "warning"}
                                    />
                                    <Typography variant="caption" color="text.secondary">
                                        Cuánto del nivel es el tono y no ruido. Por encima del {TONE_RATIO_THRESHOLD} % la medida es fiable.
                                    </Typography>
                                </Box>

                                <Box>
                                    <Typography variant="body2">
                                        Nivel de entrada: <b>{((peak / PEAK_FULL_SCALE) * 100).toFixed(0)} %</b>
                                        {headroom > 0 && !saturating ? ` · margen ×${headroom.toFixed(1)}` : ""}
                                    </Typography>
                                    <LinearProgress
                                        variant="determinate"
                                        value={Math.min(100, (peak / PEAK_FULL_SCALE) * 100)}
                                        color={saturating ? "error" : "primary"}
                                    />
                                </Box>

                                {saturating && (
                                    <Alert severity="error">
                                        El micrófono está saturando: la amplitud deja de ser fiable.
                                        Baja el volumen o la ganancia del preamplificador.
                                    </Alert>
                                )}
                                {!saturating && headroom > 0 && headroom < 4 && (
                                    <Alert severity="warning">
                                        Poco margen (×{headroom.toFixed(1)}). Al entrar en resonancia la amplitud
                                        se multiplica y puede recortar. Ajusta fuera de resonancia dejando ×4.
                                    </Alert>
                                )}
                            </Stack>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Audio reconstruido ---------------- */}
                <Grid size={{ xs: 12, md: 6 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS, height: "100%" }}>
                        <CardContent>
                            <CardTitle label="Escuchar" icon={<VolumeUpIcon />} />

                            <Stack spacing={2} sx={{ marginTop: 2 }}>
                                <ButtonCB
                                    variant="contained"
                                    color={listening ? "error" : "primary"}
                                    startIcon={listening ? <StopIcon /> : <PlayArrowIcon />}
                                    onClick={() => setListening((v) => !v)}
                                >
                                    {listening ? "Detener" : "Escuchar el tubo"}
                                </ButtonCB>

                                <Alert severity="info">
                                    Es una <b>reconstrucción</b>. El micrófono envía la amplitud medida, no la onda:
                                    el navegador sintetiza un tono a {frequency} Hz y sigue esa amplitud.
                                    Oirás cómo sube y baja al pasar por las resonancias, pero no el timbre real del tubo.
                                </Alert>
                            </Stack>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Curva de resonancia ---------------- */}
                <Grid size={{ xs: 12 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS }}>
                        <CardContent>
                            <Stack direction="row" spacing={2} alignItems="center" justifyContent="space-between" flexWrap="wrap" useFlexGap>
                                <CardTitle label="Curva de resonancia" icon={<GraphicEqIcon />} />
                                <ButtonCB variant="outlined" startIcon={<DeleteSweepIcon />} onClick={() => setSamples([])}>
                                    Limpiar
                                </ButtonCB>
                            </Stack>

                            <Box sx={{ height: 360, marginTop: 2 }}>
                                {samples.length === 0 ? (
                                    <Typography variant="body2" color="text.secondary" sx={{ padding: 4 }}>
                                        Aún no hay medidas. Mueve el émbolo y la curva se irá construyendo:
                                        cada punto es una amplitud medida en una posición.
                                    </Typography>
                                ) : (
                                    <ResponsiveLine
                                        data={chartData}
                                        margin={{ top: 30, right: 40, bottom: 70, left: 70 }}
                                        axisBottom={{ legend: "Posición del émbolo [cm]", legendOffset: 40, legendPosition: "middle" }}
                                        axisLeft={{ legend: "Amplitud", legendOffset: -55, legendPosition: "middle" }}
                                        pointSize={4}
                                        useMesh={true}
                                        theme={generatePlotTheme(theme)}
                                        colors={{ scheme: "category10" }}
                                        xScale={{ type: "linear", min: "auto", max: "auto" }}
                                        yScale={{ type: "linear", min: 0, max: "auto" }}
                                    />
                                )}
                            </Box>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Velocidad del sonido ---------------- */}
                <Grid size={{ xs: 12 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS }}>
                        <CardContent>
                            <CardTitle label="Velocidad del sonido" icon={<StraightenIcon />} />

                            <Grid container spacing={2} sx={{ marginTop: 1 }}>
                                <Grid size={{ xs: 12, md: 5 }}>
                                    <Stack spacing={1}>
                                        <Typography variant="h4">
                                            {measuredSpeed !== null ? `${measuredSpeed.toFixed(1)} m/s` : "—"}
                                        </Typography>
                                        <Typography variant="caption" color="text.secondary">
                                            v = 2 · separación entre resonancias · frecuencia
                                        </Typography>
                                        <Typography variant="body2">
                                            Resonancias detectadas: <b>{resonances.length}</b>
                                        </Typography>
                                        <Typography variant="body2" color="text.secondary">
                                            A {frequency} Hz deberían estar cada {expectedSpacing.toFixed(1)} cm
                                            si la velocidad fuera {REFERENCE_SPEED_MS} m/s.
                                        </Typography>
                                        {resonances.length < 2 && (
                                            <Alert severity="info">
                                                Hacen falta al menos dos resonancias. Barre el émbolo por todo
                                                el recorrido sin saltarte tramos.
                                            </Alert>
                                        )}
                                    </Stack>
                                </Grid>

                                <Grid size={{ xs: 12, md: 7 }}>
                                    {resonanceRows.length > 0 && (
                                        <UserGrid
                                            data_rows={resonanceRows}
                                            header_cols={[
                                                { field: "orden", headerName: "Máximo", flex: 1 },
                                                { field: "posicion", headerName: "Posición [cm]", flex: 1 },
                                                { field: "separacion", headerName: "Separación [cm]", flex: 1 },
                                            ]}
                                            disable_check
                                        />
                                    )}
                                </Grid>
                            </Grid>
                        </CardContent>
                    </Card>
                </Grid>

            </Grid>
        </Stack>
    );
}

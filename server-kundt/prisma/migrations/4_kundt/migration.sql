-- CreateTable
CREATE TABLE "remote-experiments"."Kundt" (
    "id" INTEGER NOT NULL,
    "frequency" INTEGER NOT NULL DEFAULT 0,
    "volume" INTEGER NOT NULL DEFAULT 0,
    "plunger_pos" DOUBLE PRECISION NOT NULL DEFAULT 0,
    "plunger_actual" DOUBLE PRECISION NOT NULL DEFAULT 0,
    "clockwise_limit" BOOLEAN NOT NULL DEFAULT false,
    "counter_limit" BOOLEAN NOT NULL DEFAULT false,
    "mic_amplitude" DOUBLE PRECISION NOT NULL DEFAULT 0,
    "mic_rms" DOUBLE PRECISION NOT NULL DEFAULT 0,
    "mic_peak" INTEGER NOT NULL DEFAULT 0,

    CONSTRAINT "Kundt_pkey" PRIMARY KEY ("id")
);

-- AddForeignKey
ALTER TABLE "remote-experiments"."Kundt" ADD CONSTRAINT "Kundt_id_fkey" FOREIGN KEY ("id") REFERENCES "remote-experiments"."ExpBase"("id") ON DELETE RESTRICT ON UPDATE CASCADE;

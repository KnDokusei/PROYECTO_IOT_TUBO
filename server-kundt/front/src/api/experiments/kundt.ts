import type { KundtActuators, KundtStatusDTO } from "../../types/experiments/kundt";
import api from "../axios";
import { GET_KUNDT_FROM_PLATFORM_ID, SET_KUNDT_FROM_PLATFORM_ID } from "../routes";

export const getKundtFromPlatform = async (platform_id: number): Promise<KundtStatusDTO> => {
    try {
        const response = await api.get(GET_KUNDT_FROM_PLATFORM_ID + platform_id);
        return response.data;
    }
    catch (e) {
        console.error("Error while fetching kundt platform: ", e);
        throw new Error("Server error - could not fetch kundt from platform " + platform_id);
    }
}

/*
 * Partial bodies are the norm here: sending only { frequency } leaves volume and
 * plunger_pos untouched, because the server writes undefined fields as no-ops.
 */
export const updateKundt = async (platform_id: number, body: KundtActuators): Promise<{ id: number, actuators: KundtActuators }> => {
    try {
        const response = await api.post(SET_KUNDT_FROM_PLATFORM_ID + platform_id, body);
        return response.data;
    }
    catch (e) {
        console.error("Error while updating kundt information: ", e);
        throw new Error("Server error - could not update kundt status");
    }
}

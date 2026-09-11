import type { LoaderFunctionArgs } from "react-router";
import { getExperimentPlatform } from "../../../api/experiments/experiments";
import { getKundtFromPlatform } from "../../../api/experiments/kundt";

export const validateKundtPlatform = async ({ params }: LoaderFunctionArgs) => {
    const platform_id = params.id;
    if (!platform_id || !Number(platform_id)) throw new Response("Missing platform_id", { status: 400 });

    const platform = await getExperimentPlatform(Number(platform_id));
    if (!platform.status) throw new Response("Platform is currently disabled", { status: 400 });

    const kundt_platform = await getKundtFromPlatform(Number(platform_id));

    return kundt_platform;
}

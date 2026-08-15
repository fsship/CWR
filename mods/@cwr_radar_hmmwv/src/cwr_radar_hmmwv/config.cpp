class CfgPatches
{
    class CWR_Radar_HMMWV
    {
        units[] = {"CWR_RadarHMMWV"};
        weapons[] = {"CAS_TaticMissileB"};
        requiredVersion = 1.99;
        // CAS_TaticMissile is supplied by the CAS_AH31A resource PBO.
        // Declaring the dependency unlocks that existing definition before
        // this vehicle's weapon bank is initialized.
        requiredAddons[] = {"HMMWV", "CAS_AH31A"};
    };
};

// The addon loader parses every PBO independently before it merges the
// resulting configs.  Keep this inheritance spine explicit so parsing B does
// not replace the original CAS_TaticMissile class with an empty declaration.
// B duplicates the original TaticMissile overrides and only narrows its
// indirect-damage radius from 32 m to 0.5 m.
class CfgAmmo
{
    class Default {};
    class AT3: Default {};
    class CarlGustav: AT3 {};
    class AA: CarlGustav {};
    class CAS_TaticMissileB: AA
    {
        model = "\CAS_AH31A\CAS_Aim9.p3d";
        proxyShape = "\CAS_AH31A\CAS_Aim9_Proxy.p3d";
        maneuvrability = 100;
        airLock = 1;
        irLock = 0;
        laserLock = 0;
        initTime = 0.1;
        maxLeadSpeed = 4000;
        thrustTime = 500;
        manualControl = 1;
        maxControlRange = 9000;
        thrust = 400;
        maxSpeed = 800;
        hit = 5000;
        indirectHit = 1000;
        indirectHitRange = 0.5;
        soundEngine[] = {"\CAS_ah31a\aim9flying.wav", 1, 0};
    };
};

class CfgWeapons
{
    class Default {};
    class LAWLauncher: Default {};
    class CarlGustavLauncher: LAWLauncher {};
    class AT3Launcher: CarlGustavLauncher {};
    class HellfireLauncher: AT3Launcher {};
    class MaverickLauncher: HellfireLauncher {};
    class CAS_TaticMissileB: MaverickLauncher
    {
        ammo = "CAS_TaticMissileB";
        displayName = "TaticMissileB";
        displayNameMagazine = "TaticMissileB";
        shortNameMagazine = "TaticMissileB";
        count = 60;
        initSpeed = 30;
        sound[] = {"\CAS_ah31a\aim9fire.wav", 4, 0};
        canLock = 2;
    };
};

class CfgVehicles
{
    // Addon configs are parsed independently before being merged into the
    // global config, so repeat the lightweight inheritance spine used by the
    // original humr.pbo. Existing definitions are updated, not replaced.
    class All {};
    class AllVehicles: All {};
    class Land: AllVehicles {};
    class LandVehicle: Land {};
    class Car: LandVehicle {};
    class Jeep: Car {};
    class HMMWV: Jeep {};
    class CWR_RadarHMMWV: HMMWV
    {
        scope = 2;
        side = 1;
        displayName = "HMMWV Radar / TaticMissile";
        vehicleClass = "Armored";
        picture = "\humr\ihmmwv.paa";

        armor = 120;
        cost = 650000;
        threat[] = {0.8, 0.95, 0.7};
        transportSoldier = 1;

        // Car has no unit-info overlay. Reuse the tank overlay so its tactical
        // radar is visible, and make the driver the vehicle commander.
        unitInfoType = 1;
        driverIsCommander = 1;
        commanderCanSee = 31;
        driverCanSee = 31;

        // Both variants are independently selectable and retain their own
        // inherited magazine counts. TaticMissileB differs only in splash
        // radius (0.5 m rather than the original 32 m).
        weapons[] = {"CAS_TaticMissile", "CAS_TaticMissileB"};
        magazines[] = {"CAS_TaticMissile", "CAS_TaticMissileB"};
        mountedMaverickRack = 1;
        mountedRackObliqueModel = "\cwr_radar_hmmwv\cwr_radar_rack_45.p3d";
        mountedRackVerticalModel = "\cwr_radar_hmmwv\cwr_radar_rack_90.p3d";

        // Functional ground-search radar / laser designation capability.
        irScanRangeMin = 500;
        irScanRangeMax = 3000;
        irScanToEyeFactor = 3;
        irScanGround = 1;
        // Dedicated all-aspect radar. Its fixed range and contact handling
        // are local to this vehicle; no global vehicle or infantry config is
        // modified.
        // Clear terrain line: 12 km. Terrain-masked contacts deliberately
        // fall back to a 500 m near-field search instead.
        radarRange = 12000;
        radarIgnoreLOS = 1;
        radarLockInfantry = 1;
        radarTerrainMaskedRange = 500;
        // Blue live-missile marker and launch-vehicle distance on the HUD.
        radarMissileHud = 1;
        // Terrain-masked 45-degree launches retain a longer straight run.
        // Vertical launches instead wait until they have cleared the sampled
        // terrain profile by this many metres before guidance begins.
        radarTerrainMaskUnguidedMultiplier = 3;
        radarTerrainMaskClearance = 75;
        // This sensor alone may acquire soldiers without globally making
        // infantry (or any other unit type) an IR target.
        radarScanInfantry = 1;
        laserScanner = 1;

        class Animations
        {
            class LauncherElevation
            {
                // The radar/launcher pack is rendered as an external model,
                // so retain an ordinary named animation state without trying
                // to animate selections in the stock HMMWV model.
                type = "state";
                animPeriod = 0.8;
            };
        };

        class EventHandlers
        {
            // The action-menu state is intentionally client-local. The Fired
            // handler likewise runs where the player fired the weapon, so this
            // does not create or control cameras for other players.
            init = "CWRRadarTrackCamEnabled = 0; CWRRadarTrackCamActive = 0";
            fired = "_this exec ""\cwr_radar_hmmwv\scripts\track_cam_fired.sqs""";
        };

        class UserActions
        {
            class LaunchersVertical
            {
                displayName = "Launchers: vertical (90 deg)";
                position = "pos driver";
                radius = 3;
                condition = "(player == driver this) && (alive this) && (this animationPhase ""LauncherElevation"" < 0.5)";
                statement = "this animate [""LauncherElevation"", 1]";
            };

            class LaunchersOblique
            {
                displayName = "Launchers: oblique (45 deg)";
                position = "pos driver";
                radius = 3;
                condition = "(player == driver this) && (alive this) && (this animationPhase ""LauncherElevation"" >= 0.5)";
                statement = "this animate [""LauncherElevation"", 0]";
            };

            class TrackCamEnable
            {
                displayName = "Track Cam: on";
                position = "pos driver";
                radius = 3;
                condition = "(player == driver this) && (alive this) && (CWRRadarTrackCamEnabled == 0)";
                statement = "CWRRadarTrackCamEnabled = 1";
            };

            class TrackCamDisable
            {
                displayName = "Track Cam: off";
                position = "pos driver";
                radius = 3;
                condition = "(player == driver this) && (alive this) && (CWRRadarTrackCamEnabled == 1)";
                statement = "CWRRadarTrackCamEnabled = 0";
            };
        };
    };
};

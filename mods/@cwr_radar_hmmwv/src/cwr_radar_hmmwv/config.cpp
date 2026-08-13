class CfgPatches
{
    class CWR_Radar_HMMWV
    {
        units[] = {"CWR_RadarHMMWV"};
        weapons[] = {};
        requiredVersion = 1.99;
        requiredAddons[] = {"HMMWV"};
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
        displayName = "HMMWV Radar / Maverick";
        vehicleClass = "Armored";
        model = "\cwr_radar_hmmwv\cwr_radar_hmmwv.p3d";
        picture = "\humr\ihmmwv.paa";

        armor = 120;
        cost = 650000;
        threat[] = {0.8, 0.95, 0.7};
        transportSoldier = 1;

        weapons[] = {"MaverickLauncher"};
        magazines[] = {"MaverickLauncher"};

        // Functional ground-search radar / laser designation capability.
        irScanRangeMin = 500;
        irScanRangeMax = 12000;
        irScanToEyeFactor = 3;
        irScanGround = 1;
        laserScanner = 1;

        class Animations
        {
            class LauncherElevation
            {
                type = "rotation";
                animPeriod = 0.8;
                selection = "launcher_bank";
                axis = "launcher_axis";
                angle0 = 0;
                // The model is authored at 45 degrees. In Poseidon's model
                // rotation convention, -45 degrees around +X points it upward.
                angle1 = -0.785398163;
            };
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
        };
    };
};

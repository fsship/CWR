class CfgPatches
{
    class CWR_Radar_HMMWV
    {
        units[] = {"CWR_RadarHMMWV"};
        weapons[] = {};
        requiredVersion = 1.99;
        // CAS_TaticMissile is supplied by the CAS_AH31A resource PBO.
        // Declaring the dependency unlocks that existing definition before
        // this vehicle's weapon bank is initialized.
        requiredAddons[] = {"HMMWV", "CAS_AH31A"};
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
    // Vanilla Tank deliberately disables IR targeting.  A radar-equipped
    // launcher needs armour to participate in the lockable target list.
    class Tank: LandVehicle
    {
        irTarget = 1;
    };

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

        weapons[] = {"CAS_TaticMissile"};
        magazines[] = {"CAS_TaticMissile"};
        mountedMaverickRack = 1;
        mountedRackObliqueModel = "\cwr_radar_hmmwv\cwr_radar_rack_45.p3d";
        mountedRackVerticalModel = "\cwr_radar_hmmwv\cwr_radar_rack_90.p3d";

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
                // The radar/launcher pack is rendered as an external model,
                // so retain an ordinary named animation state without trying
                // to animate selections in the stock HMMWV model.
                type = "state";
                animPeriod = 0.8;
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

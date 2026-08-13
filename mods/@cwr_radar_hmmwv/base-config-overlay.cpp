// Activate the resource-pack definition in every mission. No CAS weapon or
// ammunition properties are declared here.
class CfgAddons
{
    class PreloadAddons
    {
        class CWRRadarHMMWCAS
        {
            list[] = {"CAS_AH31A"};
        };
    };
};

class CfgVehicles
{
    // Preserve the original Man -> Land inheritance. A declaration without
    // ': Land' makes the binary merger clear that parent relationship, which
    // breaks normal infantry camera/view settings.
    class All {};
    class AllVehicles: All {};
    class Land: AllVehicles {};
    class Man: Land
    {
        class UserActions
        {
            class cheatHMMWV
            {
                displayName = "Add Radar HMMWV";
                statement = "godtx1=""CWR_RadarHMMWV"" createvehicle getpos player; if (locked godtx1) then {godtx1 lock false};player moveInDriver godtx1";
            };
        };
    };
};

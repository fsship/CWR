#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/World/Simulation/Animation/Animation.hpp>
#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>

#include "../../test_fixtures.hpp"

#include <cstring>
#include <memory>

TEST_CASE("animation.hpp - compile check", "[simulation][animation]")
{
    SUCCEED("animation.hpp included successfully");
}

TEST_CASE("rtAnimation.hpp - compile check", "[simulation][rtAnimation]")
{
    SUCCEED("rtAnimation.hpp included successfully");
}

TEST_CASE("config rotation applies the same transform to a proxy matrix", "[simulation][animation][proxy]")
{
    auto model = Poseidon::Asset::Formats::MLODLoader::load(GET_FIXTURE("mlod/animated_marker_mlod.p3d"));
    std::unique_ptr<LODShapeWithShadow> shape(Poseidon::Model::ShapeAdapter::convertToLODShape(model, false));
    REQUIRE(shape);

    const char* config = R"(
        class LauncherElevation
        {
            type = "rotation";
            animPeriod = 0.8;
            selection = "body";
            axis = "body";
            memory = 0;
            angle0 = 0;
            angle1 = 1.570796327;
        };
    )";
    ParamFile params;
    QIStream input(config, static_cast<int>(std::strlen(config)));
    params.Parse(input);

    Poseidon::AnimationRotationType animation;
    animation.Init(params >> "LauncherElevation", shape.get());

    Matrix4 original(MIdentity);
    original.SetPosition(Vector3(0.25f, 0.5f, 0.75f));

    Poseidon::AnimationRotation rawRotation;
    rawRotation.Init(shape.get(), "body", nullptr, "body", nullptr, false);
    Matrix4 expectedRotation;
    rawRotation.GetRotation(expectedRotation, H_PI / 2.0f, 0);
    Matrix4 expected = expectedRotation * original;

    Matrix4 actual = original;
    animation.AnimateMatrix(actual, 1.0f, 0);

    REQUIRE(actual.Position().Distance(expected.Position()) == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(actual.Direction().Distance(expected.Direction()) == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(actual.DirectionUp().Distance(expected.DirectionUp()) == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(actual.DirectionAside().Distance(expected.DirectionAside()) == Catch::Approx(0.0f).margin(1e-5f));
}

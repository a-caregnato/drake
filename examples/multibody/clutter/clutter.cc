#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <utility>

#include <gflags/gflags.h>

#include "drake/common/nice_type_name.h"
#include "drake/common/temp_directory.h"
#include "drake/geometry/drake_visualizer.h"
#include "drake/geometry/scene_graph.h"
#include "drake/lcm/drake_lcm.h"
#include "drake/math/random_rotation.h"
#include "drake/math/rigid_transform.h"
#include "drake/math/roll_pitch_yaw.h"
#include "drake/math/rotation_matrix.h"
#include "drake/multibody/contact_solvers/sap/sap_solver.h"
#include "drake/multibody/plant/compliant_contact_manager.h"
#include "drake/multibody/plant/contact_results_to_lcm.h"
#include "drake/systems/analysis/implicit_integrator.h"
#include "drake/systems/analysis/simulator.h"
#include "drake/systems/analysis/simulator_gflags.h"
#include "drake/systems/analysis/simulator_print_stats.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/geometry/proximity_properties.h"
#include "drake/geometry/collision_filter_declaration.h"

// To profile with Valgrind run with (the defaults are good):
// valgrind --tool=callgrind --separate-callers=10 --instr-atstart=no
// bazel-bin/examples/multibody/mp_convex_solver/clutter
#include <valgrind/callgrind.h>

#include <iostream>
#define PRINT_VAR(a) std::cout << #a ": " << a << std::endl;
#define PRINT_VARn(a) std::cout << #a ":\n" << a << std::endl;


namespace drake {
namespace multibody {
namespace examples {
namespace mp_convex_solver {
namespace {

// Simulation parameters.
DEFINE_double(simulation_time, 10.0, "Simulation duration in seconds");
DEFINE_double(
    mbp_time_step, 1.0E-2,
    "If mbp_time_step > 0, the fixed-time step period (in seconds) of discrete "
    "updates for the plant (modeled as a discrete system). "
    "If mbp_time_step = 0, the plant is modeled as a continuous system "
    "and no contact forces are displayed.  mbp_time_step must be >= 0.");
// For this demo, penetration_allowance and stiction_tolerance are only used
// when either continuous integration or TAMSI are used.
DEFINE_double(penetration_allowance, 1.0E-4, "Allowable penetration (meters).");
DEFINE_double(stiction_tolerance, 1.0E-4,
              "Allowable drift speed during stiction (m/s).");

// Physical parameters.
DEFINE_double(density, 1000.0, "The density of all objects, in kg/m³.");
DEFINE_double(friction_coefficient, 1.0,
              "All friction coefficients have this value.");
DEFINE_double(box_stiffness, 1.0e8, "Box point contact stiffness in N/m.");
DEFINE_double(sphere_stiffness, 1.0e8,
              "Sphere point contact stiffness in N/m.");
DEFINE_double(dissipation_time_constant, 0.01,
              "Dissipation time constant in seconds.");

// Contact geometry parameters.
DEFINE_bool(
    emulate_box_multicontact, true,
    "Emulate multicontact by adding spheres to the faces of box geometries.");
DEFINE_int32(
    num_spheres_per_face, 3,
    "Multi-contact emulation. We place num_sphere x num_spheres_per_face on "
    "each box face, when emulate_box_multicontact = true.");
DEFINE_bool(enable_box_box_collision, false, "Enable box vs. box contact.");
DEFINE_bool(add_box_corners, false,
            "Adds collision points at the corners of each box.");

// Scenario parameters.
DEFINE_int32(objects_per_pile, 5, "Number of objects per pile.");
DEFINE_double(dz, 0.15, "Initial distance between objects in the pile.");
DEFINE_double(scale_factor, 1.0, "Multiplicative factor to generate the pile.");
DEFINE_bool(add_sink_walls, true, "Adds wall of a sink model.");

// Visualization.
DEFINE_bool(visualize, true, "Whether to visualize (true) or not (false).");
DEFINE_bool(visualize_forces, false,
            "Whether to visualize forces (true) or not (false).");
DEFINE_bool(visualize_multicontact, false,
            "Whether to visualize (true) or not the spheres used to emulate "
            "multicontact.");
DEFINE_double(viz_period, 1.0 / 60.0, "Viz period.");

// Discrete contact solver.
DEFINE_bool(tamsi, false, "Use TAMSI (true) or MpConvexSolver (false).");
// If using Gurobi, compile with: bazel run --config gurobi ...
// If using Mosek, compile with: bazel test --config mosek ...
// To have both, simply do: bazel run --config=gurobi --config=mosek ...
//DEFINE_string(solver, "primal",
//  "Underlying solver. 'gurobi', 'mosek', 'scs', 'conex', 'primal'");
//DEFINE_bool(use_supernodal, true,
//            "Use supernodal algebra (true) or dense algebra (false).");
DEFINE_int32(verbosity_level, 0,
             "Verbosity level of the new primal solver. See "
             "UnconstrainedPrimalSolverParameters.");
DEFINE_string(line_search, "exact",
              "Primal solver line-search. 'exact', 'inexact'");
DEFINE_double(rt_factor, 1.0e-3, "Rt_factor");
DEFINE_double(alpha, 1.0, "Rigid time scale factor.");
DEFINE_double(sigma, 1.0e-3, "Friction dimensionless parameterization.");
DEFINE_double(abs_tol, 1.0e-6, "Absolute tolerance [m/s].");
DEFINE_double(rel_tol, 1.0e-5, "Relative tolerance [-].");
DEFINE_bool(log_stats, true, "Log all iterations stats.");
DEFINE_bool(log_cond_number, false,
            "Estimate and log condition number (expensive).");

DEFINE_bool(ls_exact, true, "ls_exact");
DEFINE_double(ls_alpha_max, 1.25, "ls_alpha_max");
DEFINE_double(ls_rho, 0.8, "ls_rho");

using drake::math::RigidTransform;
using drake::math::RigidTransformd;
using drake::math::RollPitchYawd;
using drake::math::RotationMatrixd;
using drake::multibody::ContactResults;
using drake::multibody::MultibodyPlant;
using drake::multibody::internal::CompliantContactManager;
using drake::multibody::internal::ManagerStats;
using drake::multibody::contact_solvers::internal::SapSolver;
using drake::multibody::contact_solvers::internal::SapSolverParameters;
using drake::geometry::CollisionFilterDeclaration;
using Eigen::Translation3d;
using Eigen::Vector3d;
using clock = std::chrono::steady_clock;

// Parameters
const double width(0.8);
const double length(0.8);

std::vector<geometry::GeometryId> box_geometry_ids;

const RigidBody<double>& AddBox(const std::string& name,
                                const Vector3<double>& block_dimensions,
                                double mass, double stiffness, double friction,
                                const Vector4<double>& color,
                                bool emulate_box_multicontact,
                                bool add_box_collision,
                                MultibodyPlant<double>* plant) {
  // Ensure the block's dimensions are mass are positive.
  const double LBx = block_dimensions.x();
  const double LBy = block_dimensions.y();
  const double LBz = block_dimensions.z();

  // Describe body B's mass, center of mass, and inertia properties.
  const Vector3<double> p_BoBcm_B = Vector3<double>::Zero();
  const UnitInertia<double> G_BBcm_B =
      UnitInertia<double>::SolidBox(LBx, LBy, LBz);
  const SpatialInertia<double> M_BBcm_B(mass, p_BoBcm_B, G_BBcm_B);

  // Create a rigid body B with the mass properties of a uniform solid block.
  const RigidBody<double>& box = plant->AddRigidBody(name, M_BBcm_B);

  // Box's visual.
  // The pose X_BG of block B's geometry frame G is an identity transform.
  const RigidTransform<double> X_BG;  // Identity transform.
  plant->RegisterVisualGeometry(box, X_BG, geometry::Box(LBx, LBy, LBz),
                                name + "_visual", color);

  // When the TAMSI solver is used, we simply let MultibodyPlant estimate
  // contact parameters based on penetration_allowance and stiction_tolerance.
  geometry::ProximityProperties props;
  if (!FLAGS_tamsi || FLAGS_mbp_time_step == 0) {
    props.AddProperty(geometry::internal::kMaterialGroup,
                      geometry::internal::kPointStiffness, stiffness);
    props.AddProperty(geometry::internal::kMaterialGroup,
                      "dissipation_time_constant",
                      FLAGS_dissipation_time_constant);
  }
  props.AddProperty(geometry::internal::kMaterialGroup,
                    geometry::internal::kFriction,
                    CoulombFriction<double>(friction, friction));

  // Box's collision geometry is a solid box.
  if (emulate_box_multicontact) {
    const Vector4<double> red(1.0, 0.0, 0.0, 1.0);
    const Vector4<double> red_50(1.0, 0.0, 0.0, 0.5);
    const double radius_x = LBx / FLAGS_num_spheres_per_face / 2.0;
    const double radius_y = LBy / FLAGS_num_spheres_per_face / 2.0;
    const double radius_z = LBz / FLAGS_num_spheres_per_face / 2.0;
    double dx = 2 * radius_x;
    double dy = 2 * radius_y;
    double dz = 2 * radius_z;
    const int ns = FLAGS_num_spheres_per_face;

    auto add_sphere = [&](const std::string& sphere_name, double x, double y,
                          double z, double radius) {
      const Vector3<double> p_BoSpherei_B(x, y, z);
      const RigidTransform<double> X_BSpherei(p_BoSpherei_B);
      geometry::Sphere shape(radius);
      // Ellipsoid might not be accurate. From console [warning]:
      // "Ellipsoid is primarily for ComputeContactSurfaces in
      // hydroelastic contact model. The accuracy of other collision
      // queries and signed distance queries are not guaranteed."
      // geometry::Ellipsoid shape(radius_x, radius_y, radius_z);
      plant->RegisterCollisionGeometry(box, X_BSpherei, shape, sphere_name,
                                       props);
      if (FLAGS_visualize_multicontact) {
        plant->RegisterVisualGeometry(box, X_BSpherei, shape, sphere_name, red);
      }
    };

    // Add points (zero size spheres) at the corners to avoid spurious
    // interpentrations between boxes and the sink.
    if (FLAGS_add_box_corners) {
      add_sphere("c1", -LBx / 2, -LBy / 2, -LBz / 2, 0);
      add_sphere("c2", +LBx / 2, -LBy / 2, -LBz / 2, 0);
      add_sphere("c3", -LBx / 2, +LBy / 2, -LBz / 2, 0);
      add_sphere("c4", +LBx / 2, +LBy / 2, -LBz / 2, 0);
      add_sphere("c5", -LBx / 2, -LBy / 2, +LBz / 2, 0);
      add_sphere("c6", +LBx / 2, -LBy / 2, +LBz / 2, 0);
      add_sphere("c7", -LBx / 2, +LBy / 2, +LBz / 2, 0);
      add_sphere("c8", +LBx / 2, +LBy / 2, +LBz / 2, 0);
    }

    // Make a "mesh" of non-zero radii spheres.
    for (int i = 0; i < ns; ++i) {
      const double x = -LBx / 2 + radius_x + i * dx;
      for (int j = 0; j < ns; ++j) {
        const double y = -LBy / 2 + radius_y + j * dy;
        for (int k = 0; k < ns; ++k) {
          const double z = -LBz / 2 + radius_z + k * dz;
          if (i == 0 || j == 0 || k == 0 || i == ns - 1 || j == ns - 1 ||
              k == ns - 1) {
            const std::string name_spherei =
                fmt::format("{}_sphere_{}{}{}_collision", name, i, j, k);
            add_sphere(name_spherei, x, y, z, radius_x);
          }
        }  // k
      }    // j
    }      // i
  }

  if (add_box_collision) {
    auto id = plant->RegisterCollisionGeometry(
        box, X_BG, geometry::Box(LBx, LBy, LBz), name + "_collision", props);
    box_geometry_ids.push_back(id);
  }
  return box;
}

void AddSink(MultibodyPlant<double>* plant) {
  DRAKE_THROW_UNLESS(plant != nullptr);

  // Parameters for the sink.
  // const double length = 1.0;
  // const double width = 0.8;
  const double height = 0.8;
  const double wall_thickness = 0.04;
  const double wall_mass = 1.0;
  const double friction_coefficient = FLAGS_friction_coefficient;
  const Vector4<double> light_blue(0.5, 0.8, 1.0, 0.3);
  const Vector4<double> light_green(0., 0.7, 0.0, 1.0);

  auto add_wall =
      [&](const std::string& name, const Vector3d& dimensions,
          const RigidTransformd& X_WB,
          const Vector4<double>& color) -> const RigidBody<double>& {
    const double kSinkStiffness = 1.0e10;
    const auto& wall = AddBox(name, dimensions, wall_mass, kSinkStiffness,
                              friction_coefficient, color, false, true, plant);
    plant->WeldFrames(plant->world_frame(), wall.body_frame(), X_WB);
    return wall;
  };

  const Vector3d bottom_dimensions(50 * length, 50 * width, wall_thickness);
  const Vector3d side_wall_dimensions(height, width, wall_thickness);
  const Vector3d back_front_wall_dimensions(length, wall_thickness, height);

  add_wall("sink_bottom", bottom_dimensions,
           Translation3d(0, 0, -wall_thickness / 2.0), light_green);

  if (FLAGS_add_sink_walls) {
    add_wall("sink_right", side_wall_dimensions,
             RigidTransformd(RotationMatrixd::MakeYRotation(M_PI_2),
                             Vector3d(length / 2.0, 0.0, height / 2.0)),
             light_blue);
    add_wall("sink_left", side_wall_dimensions,
             RigidTransformd(RotationMatrixd::MakeYRotation(M_PI_2),
                             Vector3d(-length / 2.0, 0.0, height / 2.0)),
             light_blue);
    add_wall("sink_back", back_front_wall_dimensions,
             Translation3d(0.0, width / 2, height / 2), light_blue);
    add_wall("sink_front", back_front_wall_dimensions,
             Translation3d(0.0, -width / 2, height / 2), light_blue);
  }
}

const RigidBody<double>& AddSphere(const std::string& name, const double radius,
                                   double mass, double friction,
                                   const Vector4<double>& color,
                                   MultibodyPlant<double>* plant) {
  const UnitInertia<double> G_Bcm = UnitInertia<double>::SolidSphere(radius);
  const SpatialInertia<double> M_Bcm(mass, Vector3<double>::Zero(), G_Bcm);

  const RigidBody<double>& ball = plant->AddRigidBody(name, M_Bcm);

  geometry::ProximityProperties props;
  if (!FLAGS_tamsi || FLAGS_mbp_time_step == 0) {
    props.AddProperty(geometry::internal::kMaterialGroup,
                      geometry::internal::kPointStiffness, FLAGS_sphere_stiffness);
    props.AddProperty(geometry::internal::kMaterialGroup,
                      "dissipation_time_constant",
                      FLAGS_dissipation_time_constant);
  }
  props.AddProperty(geometry::internal::kMaterialGroup,
                    geometry::internal::kFriction,
                    CoulombFriction<double>(friction, friction));

  // Add collision geometry.
  const RigidTransformd X_BS = RigidTransformd::Identity();
  plant->RegisterCollisionGeometry(ball, X_BS, geometry::Sphere(radius),
                                   name + "_collision", props);

  // Add visual geometry.
  plant->RegisterVisualGeometry(ball, X_BS, geometry::Sphere(radius),
                                name + "_visual", color);

  // We add a few spots so that we can appreciate the sphere's
  // rotation, colored on red, green, blue according to the body's axes.
  const Vector4<double> red(1.0, 0.0, 0.0, 1.0);
  const Vector4<double> green(0.0, 1.0, 0.0, 1.0);
  const Vector4<double> blue(0.0, 0.0, 1.0, 1.0);
  const double visual_radius = 0.2 * radius;
  const geometry::Cylinder spot(visual_radius, visual_radius);
  // N.B. We do not place the cylinder's cap exactly on the sphere surface to
  // avoid visualization artifacts when the surfaces are kissing.
  const double radial_offset = radius - 0.45 * visual_radius;
  auto spot_pose = [](const Vector3<double>& position) {
    // The cylinder's z-axis is defined as the normalized vector from the
    // sphere's origin to the cylinder's center `position`.
    const Vector3<double> axis = position.normalized();
    return RigidTransformd(
        Eigen::Quaterniond::FromTwoVectors(Vector3<double>::UnitZ(), axis),
        position);
  };
  plant->RegisterVisualGeometry(ball, spot_pose({radial_offset, 0., 0.}), spot,
                                name + "_x+", red);
  plant->RegisterVisualGeometry(ball, spot_pose({-radial_offset, 0., 0.}), spot,
                                name + "_x-", red);
  plant->RegisterVisualGeometry(ball, spot_pose({0., radial_offset, 0.}), spot,
                                name + "_y+", green);
  plant->RegisterVisualGeometry(ball, spot_pose({0., -radial_offset, 0.}), spot,
                                name + "_y-", green);
  plant->RegisterVisualGeometry(ball, spot_pose({0., 0., radial_offset}), spot,
                                name + "_z+", blue);
  plant->RegisterVisualGeometry(ball, spot_pose({0., 0., -radial_offset}), spot,
                                name + "_z-", blue);
  return ball;
}

std::vector<BodyIndex> AddObjects(double scale_factor,
                                  MultibodyPlant<double>* plant) {
  const double radius0 = 0.05;
  const double density = FLAGS_density;  // kg/m^3.
  // const double mass = 0.2;

  const double friction = FLAGS_friction_coefficient;
  const Vector4<double> orange(1.0, 0.55, 0.0, 1.0);
  const Vector4<double> purple(204.0 / 255, 0.0, 204.0 / 255, 1.0);
  const Vector4<double> green(0, 153.0 / 255, 0, 1.0);
  const Vector4<double> cyan(51 / 255, 1.0, 1.0, 1.0);
  const Vector4<double> pink(1.0, 204.0 / 255, 204.0 / 255, 1.0);
  std::vector<Vector4<double>> colors;
  colors.push_back(orange);
  colors.push_back(purple);
  colors.push_back(green);
  colors.push_back(cyan);
  colors.push_back(pink);

  const int seed = 41;
  std::mt19937 generator(seed);
  std::uniform_int_distribution<int> distribution(0, 1);

  auto roll_shape = [&]() {
    // return 0;
    return distribution(generator);
  };

  const int num_objects = FLAGS_objects_per_pile;
  const int num_bodies = plant->num_bodies();

  std::vector<BodyIndex> bodies;
  for (int i = 1; i <= num_objects; ++i) {
    const auto& color = colors[(i - 1) % colors.size()];
    const std::string name = "object" + std::to_string(i + num_bodies);

    double e = FLAGS_scale_factor > 0 ? i - 1 : num_objects - i;
    double scale = std::pow(std::abs(FLAGS_scale_factor), e);

    switch (roll_shape()) {
      case 0: {
        const double radius = radius0 * scale;
        const double volume = 4. / 3. * M_PI * radius * radius * radius;
        const double mass = density * volume;
        bodies.push_back(
            AddSphere(name, radius, mass, friction, color, plant).index());
        break;
      }
      case 1: {
        const Vector3d box_size = 2 * radius0 * Vector3d::Ones() * scale;
        const double volume = box_size(0) * box_size(1) * box_size(2);
        const double mass = density * volume;
        Vector4<double> color50(color);
        color50.z() = 0.5;
        bodies.push_back(AddBox(name, box_size, mass, FLAGS_box_stiffness,
                                friction, color50,
                                FLAGS_emulate_box_multicontact, true, plant)
                             .index());
        break;
      }
    }
    scale *= scale_factor;
  }

  return bodies;
}

void SetObjectsIntoAPile(const MultibodyPlant<double>& plant,
                         const Vector3d& offset,
                         const std::vector<BodyIndex>& bodies,
                         systems::Context<double>* plant_context) {
  const double delta_z = FLAGS_dz;  // assume objects have a BB of about 10 cm.

  const int seed = 41;
  std::mt19937 generator(seed);

  int num_objects = FLAGS_objects_per_pile;

  double z = delta_z / 2;
  int i = 1;
  for (auto body_index : bodies) {
    const auto& body = plant.get_body(body_index);
    if (body.is_floating()) {
      double e = FLAGS_scale_factor > 0 ? i - 1 : num_objects - i;
      double scale = std::pow(std::abs(FLAGS_scale_factor), e);

      const RotationMatrixd R_WB =
          math::UniformlyRandomRotationMatrix<double>(&generator);
      const Vector3d p_WB = offset + Vector3d(0.0, 0.0, z);

      plant.SetFreeBodyPose(plant_context, body, RigidTransformd(R_WB, p_WB));
      z += delta_z * scale;
      ++i;
    }
  }
}

int do_main() {
  // Build a generic multibody plant.
  systems::DiagramBuilder<double> builder;
  auto [plant, scene_graph] =
      AddMultibodyPlantSceneGraph(&builder, FLAGS_mbp_time_step);

  AddSink(&plant);

  // AddSphere("sphere", radius, mass, friction, orange, &plant);
  auto pile1 = AddObjects(FLAGS_scale_factor, &plant);
  auto pile2 = AddObjects(FLAGS_scale_factor, &plant);
  auto pile3 = AddObjects(FLAGS_scale_factor, &plant);
  auto pile4 = AddObjects(FLAGS_scale_factor, &plant);

  // Only box-sphere and sphere-sphere are allowed.
  if (!FLAGS_enable_box_box_collision) {
    geometry::GeometrySet all_boxes(box_geometry_ids);
    scene_graph.collision_filter_manager().Apply(
        CollisionFilterDeclaration().ExcludeWithin(all_boxes));
  }

  plant.Finalize();

  if (FLAGS_tamsi || FLAGS_mbp_time_step == 0) {
    plant.set_penetration_allowance(FLAGS_penetration_allowance);
    plant.set_stiction_tolerance(FLAGS_stiction_tolerance);
  }
  
  CompliantContactManager<double>* manager{nullptr};
  if (!FLAGS_tamsi) {
    auto owned_contact_manager =
      std::make_unique<CompliantContactManager<double>>();
    manager = owned_contact_manager.get();
    (void)manager;
    plant.SetDiscreteUpdateManager(std::move(owned_contact_manager));  
    SapSolverParameters parameters;
    parameters.exact_line_search = FLAGS_ls_exact;
    parameters.ls_alpha_max = FLAGS_ls_alpha_max;
    parameters.ls_rho = FLAGS_ls_rho;
    manager->set_sap_solver_parameters(parameters);
  }

  fmt::print("Num positions: {:d}\n", plant.num_positions());
  fmt::print("Num velocities: {:d}\n", plant.num_velocities());

  // Publish contact results for visualization.
  if (FLAGS_visualize) {
    geometry::DrakeVisualizerParams viz_params;
    viz_params.publish_period = FLAGS_viz_period;
    geometry::DrakeVisualizerd::AddToBuilder(&builder, scene_graph, nullptr,
                                             viz_params);
  }
  if (FLAGS_visualize_forces) {
    ConnectContactResultsToDrakeVisualizer(&builder, plant, scene_graph);
  }
  auto diagram = builder.Build();

  // Create a context for this system:
  std::unique_ptr<systems::Context<double>> diagram_context =
      diagram->CreateDefaultContext();
  diagram->SetDefaultContext(diagram_context.get());
  systems::Context<double>& plant_context =
      diagram->GetMutableSubsystemContext(plant, diagram_context.get());

  // In the plant's default context, we assume the state of body B in world W is
  // such that X_WB is an identity transform and B's spatial velocity is zero.
  plant.SetDefaultContext(&plant_context);

  SetObjectsIntoAPile(plant, Vector3d(length / 4, width / 4, 0), pile1,
                      &plant_context);

  SetObjectsIntoAPile(plant, Vector3d(-length / 4, width / 4, 0), pile2,
                      &plant_context);

  SetObjectsIntoAPile(plant, Vector3d(-length / 4, -width / 4, 0), pile3,
                      &plant_context);
  SetObjectsIntoAPile(plant, Vector3d(length / 4, -width / 4, 0), pile4,
                      &plant_context);

  auto simulator =
      MakeSimulatorFromGflags(*diagram, std::move(diagram_context));

  clock::time_point sim_start_time = clock::now();
  CALLGRIND_START_INSTRUMENTATION;
  simulator->AdvanceTo(FLAGS_simulation_time);
  CALLGRIND_STOP_INSTRUMENTATION;
  clock::time_point sim_end_time = clock::now();
  const double sim_time =
      std::chrono::duration<double>(sim_end_time - sim_start_time).count();
  std::cout << "AdvanceTo() time [sec]: " << sim_time << std::endl;

  PrintSimulatorStatistics(*simulator);

  const ManagerStats& stats = manager->stats();
  PRINT_VAR(stats.free_motion_accelerations_time);
  PRINT_VAR(stats.free_motion_velocities_time);
  PRINT_VAR(stats.discrete_pairs_time);
  PRINT_VAR(stats.contact_kinematics_time);
  PRINT_VAR(stats.make_problem_time);
  PRINT_VAR(stats.solve_problem_time);
  PRINT_VAR(stats.pack_results_time);
  PRINT_VAR(stats.discrete_update_time);
  PRINT_VAR(stats.contact_results_time);

  std::cout << std::endl;
  PRINT_VAR(stats.sap_stats.size());
  PRINT_VAR(stats.num_iters);
  PRINT_VAR(stats.num_ls_iters);

  // Average number of constraints.
#if 0
  int num_constraints = 0;
  int num_constraint_equations = 0;
  for (const auto& s : stats.sap_stats) {
    num_constraints += s.num_constraints;
    num_constraint_equations += s.num_constraint_equations;
  }
  PRINT_VAR(num_constraints);
  PRINT_VAR(num_constraint_equations);
#endif
  return 0;
}

}  // namespace
}  // namespace mp_convex_solver
}  // namespace examples
}  // namespace multibody
}  // namespace drake

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage(
      "\nSimulation of a clutter of objects falling into a box container.");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  return drake::multibody::examples::mp_convex_solver::do_main();
}
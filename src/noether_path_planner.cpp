#include "godel_noether/noether_path_planner.h"

#include <pluginlib/class_list_macros.h>
#include <ros/console.h>
#include <noether_conversions/noether_conversions.h>
#include <vtk_viewer/vtk_utils.h>
#include <path_sequence_planner/simple_path_sequence_planner.h>
#include <eigen_conversions/eigen_msg.h>
#include <Eigen/Dense>
#include <ros/node_handle.h>
#include <eigen_stl_containers/eigen_stl_containers.h>

namespace
{

/**
 * @brief Structure to store the first and last positions of the path segments. This is all the info
 * we need to make decisions about path order as we currently don't split paths up.
 *
 * The \e a and \e b fields do not indicate any spatial relationship and are merely to uniquely
 * identify the two end points of a line segment.
 *
 * The \id field here is used to store the index of the input path that corresponds to this
 * segment. These points are sorted so this field is used to reconstruct the result at the end.
 */
struct PathEndPoints
{
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  Eigen::Vector3d a;
  Eigen::Vector3d b;
  size_t id;
};

/**
 * @brief A structure to hold the path segments and their direction. The \e id field indicates the
 * index into the PathEndPoints sequence that corresponds to this method. The \e from_a field is used
 * to indicate whether the path should go A to B or B to A. A true value indicates A to B.
 */
struct SequencePoint
{
  size_t id;
  bool from_a;
};

// Helpers to go from pose arrays to Eigen vectors of Poses
EigenSTL::vector_Affine3d toEigen(const geometry_msgs::PoseArray& p)
{
  EigenSTL::vector_Affine3d rs (p.poses.size());
  std::transform(p.poses.begin(), p.poses.end(), rs.begin(), [] (const geometry_msgs::Pose& pose)
  {
    Eigen::Affine3d e;
    tf::poseMsgToEigen(pose, e);
    return e;
  });
  return rs;
}

// Helpers to go from pose arrays to Eigen vectors of Poses
std::vector<EigenSTL::vector_Affine3d> toEigen(const std::vector<geometry_msgs::PoseArray>& ps)
{
  std::vector<EigenSTL::vector_Affine3d> rs (ps.size());
  std::transform(ps.begin(), ps.end(), rs.begin(), [] (const geometry_msgs::PoseArray& poses)
  {
    return toEigen(poses);
  });
  return rs;
}

geometry_msgs::PoseArray toMsg(const EigenSTL::vector_Affine3d& p)
{
  geometry_msgs::PoseArray msg;
  msg.poses.resize(p.size());
  std::transform(p.begin(), p.end(), msg.poses.begin(), [] (const Eigen::Affine3d& pose) {
    geometry_msgs::Pose pose_msg;
    tf::poseEigenToMsg(pose, pose_msg);
    return pose_msg;
  });
  return msg;
}

/**
 * @brief From a sequence of path segments, this method extracts the end points and puts them into
 * a new reference frame. As segments are indivisible, we only need the extremes for sorting them.
 * @param segments The source of path segment data
 * @param ref_rotation A transform from the origin to a reference frame which we want all the end
 * points in. The paths in \e segments are considered to be in the origin frame.
 * @return A sequence of end points in the reference frame of \e ref_rotation
 */
std::vector<PathEndPoints> toEndPoints(const std::vector<EigenSTL::vector_Affine3d>& segments,
                                       const Eigen::Quaterniond& ref_rotation)
{
  // Ref rotation is the Target Frame w.r.t. Origin
  // The points are all w.r.t. Origin, ergo we have to pre-multiply by the inverse of ref_rotation
  // to get the new points in the Target Frame
  Eigen::Affine3d ref_inv;
  ref_inv = ref_rotation.inverse();

  std::vector<PathEndPoints> result;
  for (std::size_t i = 0; i < segments.size(); ++i)
  {
    const auto& s = segments[i];
    Eigen::Vector3d a = (ref_inv * s.front()).translation();
    Eigen::Vector3d b = (ref_inv * s.back()).translation();
    result.push_back({a, b, i});
  }
  return result;
}

static void reversePathAndPoses(geometry_msgs::PoseArray& path)
{
  std::reverse(path.poses.begin(), path.poses.end());
  for (auto& msg : path.poses)
  {
    Eigen::Affine3d eig;
    tf::poseMsgToEigen(msg, eig);
    eig *= Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitZ());
    tf::poseEigenToMsg(eig, msg);
  }
}

/**
 * @brief Reconstructs a set of PoseArray objects using the given set of sequence points which contain
 * indices into the \e end_points array which reference the original \e in trajectory.
 * @param in The original trajectory
 * @param seqs The sequence points whose 'id' field reaches into the \e end_points vector
 * @param end_points The set of end points whose 'id' field reaches into the \e in vector
 * @return A new pose array constructed with the sequence ordering from the \e in trajectory
 */
std::vector<geometry_msgs::PoseArray> makeSequence(const std::vector<geometry_msgs::PoseArray>& in,
                                                   const std::vector<SequencePoint>& seqs,
                                                   const std::vector<PathEndPoints>& end_points)
{
  assert(in.size() == seqs.size());
  std::vector<geometry_msgs::PoseArray> rs;
  rs.reserve(in.size());

  for (const auto& seq : seqs)
  {
    rs.push_back(in[end_points[seq.id].id]); // seq.id points to end_points; end_points.id points to in
    if (!seq.from_a) // The 'in' trajectory has segments that are always A to B
    {
//      std::reverse(rs.back().poses.begin(), rs.back().poses.end());
      reversePathAndPoses(rs.back());
    }
  }

  return rs;
}

/**
 * @brief Computes the 'average' quaternion from an input set of them.
 * See http://stackoverflow.com/questions/12374087/average-of-multiple-quaternions
 * See http://www.acsu.buffalo.edu/~johnc/ave_quat07.pdf
 *
 * I don't have a great way of detecting the cases where the result isn't really meaningful,
 * e.g. a set of rotations spread evenly through rotational space.
 */
Eigen::Quaterniond average(const std::vector<Eigen::Quaterniond, Eigen::aligned_allocator<Eigen::Quaterniond>>& qs)
{
  Eigen::MatrixXd Q (4, qs.size());

  for (std::size_t i = 0; i < qs.size(); ++i)
  {
    Q.col(i) = qs[i].coeffs();
  }

  Eigen::MatrixXd Q_prime = Q * Q.transpose();

  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eigensolver(Q_prime);

  Eigen::VectorXd eigen_vals = eigensolver.eigenvalues();
  Eigen::MatrixXd eigen_vecs = eigensolver.eigenvectors();

  int max_idx = 0;
  double max_value = 0.0;
  for (int i = 0; i < eigen_vals.size(); ++i)
  {
    if (eigen_vals(i) > max_value)
    {
      max_idx = i;
      max_value = eigen_vals(i);
    }
  }

  Eigen::VectorXd coeffs = eigen_vecs.col(max_idx);
  Eigen::Quaterniond avg_quat (coeffs(3), coeffs(0), coeffs(1), coeffs(2));
  return avg_quat;
}


// Gets the average quaternion rotation of a set of poses
Eigen::Quaterniond averageQuaternion(const EigenSTL::vector_Affine3d& poses)
{
  std::vector<Eigen::Quaterniond, Eigen::aligned_allocator<Eigen::Quaterniond>> qs;
  qs.reserve(poses.size());

  for (const auto& p : poses)
  {
    qs.push_back(Eigen::Quaterniond(p.rotation()));
  }

  return average(qs);
}

/**
 * @brief Returns the index of the path segment with the largest end-point displacement
 * (first.position - last.position) in \e segments
 *
 * We assume that segments is non-empty. Will return 0 in that case.
 */
std::size_t longestSegment(const std::vector<EigenSTL::vector_Affine3d>& segments)
{
  std::size_t max_index = 0;
  double max_dist = 0.0;

  for (std::size_t i = 0; i < segments.size(); ++i)
  {
    auto dist = (segments[i].front().translation() - segments[i].back().translation()).squaredNorm();
    if (dist > max_dist)
    {
      max_index = i;
      max_dist = dist;
    }
  }
  return max_index;
}

/**
 * @brief Given \e input, a set of path segments, this algorithm will produce a new set of segments
 * that is the result of re-ordering the points left to right relative to the nominal 'cut' direction.
 */
std::vector<geometry_msgs::PoseArray> sequence(const std::vector<geometry_msgs::PoseArray>& input)
{
  if (input.empty())
  {
    return {};
  }

  auto eigen_poses = toEigen(input);
  // We need to compute the 'nominal' cut direction of the surface paths
  // We do that by picking the "largest" cut first
  auto longest_segment_idx = longestSegment(eigen_poses);
  // Then we find the average rotation
  Eigen::Quaterniond avg_quaternion = averageQuaternion(eigen_poses[longest_segment_idx]);
  // And get the end points of the path segments in that rotational frame, such that paths
  // run along the X direction and are spaced out ~ in Y
  auto end_points = toEndPoints(eigen_poses, avg_quaternion);

  // Sort end points, -y to y
  std::sort(end_points.begin(), end_points.end(), [] (const PathEndPoints& lhs, const PathEndPoints& rhs)
  {
    auto lhs_value = std::min(lhs.a.y(), lhs.b.y());
    auto rhs_value = std::min(rhs.a.y(), rhs.b.y());
    return lhs_value < rhs_value;
  });

  // A helper function to get the starting point of a transition given a sequence number and
  // whether we started at A or B.
  auto current_position = [&end_points](const SequencePoint& p) {
    if (p.from_a) // If we came from A, we're now at B
      return end_points[p.id].b;
    else // if we came from B, we're not at A
      return end_points[p.id].a;
  };

  std::vector<SequencePoint> sequence;
  sequence.reserve(input.size());

  // We always start at the first end_point, position A
  sequence.push_back({0, true});

  for (std::size_t i = 1; i < end_points.size(); ++i)
  {
    // We need to determine if A or B of the next path is closer to the current position
    const Eigen::Vector3d current_pos = current_position(sequence.back());

    const auto dist_a = (end_points[i].a - current_pos).squaredNorm();
    const auto dist_b = (end_points[i].b - current_pos).squaredNorm();

    const auto from_a = dist_a < dist_b;
    sequence.push_back({i, from_a});
  }

  // Re-order the original inputs and produce a new segment.
  return makeSequence(input, sequence, end_points);
}

std::vector<tool_path_planner::ProcessPath>
planPaths(vtkSmartPointer<vtkPolyData> mesh,
          const tool_path_planner::ProcessTool& tool)
{
  std::vector<vtkSmartPointer<vtkPolyData>> meshes;
  meshes.push_back(mesh);
  
  tool_path_planner::RasterToolPathPlanner planner;
  planner.setTool(tool);
  std::vector<std::vector<tool_path_planner::ProcessPath>> paths;
  planner.planPaths(meshes, paths);
  assert(paths.size() == 1);
  return paths.front();
}

// MARGIN CODE
double pointDistance(const Eigen::Affine3d& a, const Eigen::Affine3d& b)
{
  return (a.translation() - b.translation()).norm();
}

double segmentLength(const EigenSTL::vector_Affine3d& segment)
{
  double length = 0.0;
  for (std::size_t i = 1; i < segment.size(); ++i)
  {
    length += pointDistance(segment[i], segment[i-1]);
  }
  return length;
}

bool approxEqual(const double a, const double b, const double eps = 1e-3)
{
  return std::fabs(a - b) < eps;
}

EigenSTL::vector_Affine3d applyMargins(const EigenSTL::vector_Affine3d& segment, const double offset)
{
  const auto length = segmentLength(segment);
  if (length < 2.0 * offset)
    return segment; // return identity - don't modify this path

  // If we know our path is long enough, let's find where it should be
  double distance_to_go = offset;
  std::size_t forward_index = 0;
  double forward_dist = 0.0;

  for (std::size_t i = 1; i < segment.size(); ++i)
  {
    const auto segment_dist = pointDistance(segment[i], segment[i-1]);
    if (approxEqual(segment_dist, distance_to_go))
    {
      std::cout << "Approx equal\n";
      forward_index = i;
      forward_dist = 0.0;
      break;
    }
    else if (distance_to_go > segment_dist) {
      distance_to_go -= segment_dist;
    } else {
      // We found our point - it's between i and i-1
      forward_index = i;
      forward_dist = distance_to_go;
      std::cout << "Dist to go: " << distance_to_go << "\n";
      break;
    }
  }

  if (forward_index == 0) throw std::logic_error("Something went wrong with margins");

  // find the reverse point
  distance_to_go = offset;
  std::size_t reverse_index = segment.size() - 1;
  double reverse_dist = 0.0;

  for (std::size_t i = segment.size() - 1; i > 0; --i)
  {
    const auto idx = i - 1;
    const auto segment_dist = pointDistance(segment[idx], segment[idx+1]);
    if (approxEqual(segment_dist, distance_to_go))
    {
      reverse_index = idx;
      reverse_dist = 0.0;
      break;
    }
    else if (distance_to_go > segment_dist) {
      distance_to_go -= segment_dist;
    } else {
      reverse_index = idx;
      reverse_dist = distance_to_go;
      break;
    }
  }

  if (reverse_index == segment.size() - 1) throw std::logic_error("Something went wrong with margins (reverse)");

  auto interp = [] (const Eigen::Affine3d& start, const Eigen::Affine3d& end, double dist)
  {
    Eigen::Affine3d new_pt;
    new_pt.translation() = start.translation() + (end.translation() - start.translation()).normalized() * dist;
    new_pt.linear() = start.rotation();
    return new_pt;
  };

  Eigen::Affine3d new_start = interp(segment[forward_index-1], segment[forward_index], forward_dist);
  Eigen::Affine3d new_end = interp(segment[reverse_index+1], segment[reverse_index], reverse_dist);

  EigenSTL::vector_Affine3d new_segment;
  if (forward_dist != 0.0) new_segment.push_back(new_start);

  for (std::size_t i = forward_index; i <= reverse_index; ++i)
    new_segment.push_back(segment[i]);

  if (reverse_dist != 0.0) new_segment.push_back(new_end);
  return new_segment;
}

std::vector<geometry_msgs::PoseArray> applyMargins(const std::vector<geometry_msgs::PoseArray>& paths,
                                                   const double offset)
{
  std::vector<EigenSTL::vector_Affine3d> segments = toEigen(paths);
  std::vector<geometry_msgs::PoseArray> result;

  for (const auto& segment : segments)
  {
    auto with_margins = applyMargins(segment, offset);
    result.push_back(toMsg(with_margins));
  }
  return result;
}

} // anon namespace

void godel_noether::NoetherPathPlanner::init(pcl::PolygonMesh mesh)
{
  mesh_ = mesh;
}

bool godel_noether::NoetherPathPlanner::generatePath(
    std::vector<geometry_msgs::PoseArray> &path)
{
  ROS_INFO("Starting Noether path planning...");
  auto vtk_data = vtkSmartPointer<vtkPolyData>::New();
  vtk_viewer::pclEncodeMeshAndNormals(mesh_, vtk_data, 0.05);
  vtk_viewer::generateNormals(vtk_data);
  ROS_INFO("generatePath: converted mesh to VTK");

  auto tool = loadTool();
  auto process_paths = planPaths(vtk_data, tool);
  ROS_INFO("generatePath: finished planning paths");

  auto paths = tool_path_planner::convertVTKtoGeometryMsgs(process_paths);
  paths = applyMargins(paths, 0.25 * 0.0254);
  path = sequence(paths);

  ROS_INFO("generatePath: converted to ROS messages - DONE!");

  return true;
}

tool_path_planner::ProcessTool godel_noether::NoetherPathPlanner::loadTool() const
{
  tool_path_planner::ProcessTool tool;
  tool.pt_spacing = 0.01;
  tool.line_spacing = 0.025;
  tool.tool_offset = 0.0; // currently unused
  tool.intersecting_plane_height = 0.05; // 0.5 works best, not sure if this should be included in the tool
  tool.nearest_neighbors = 5; // not sure if this should be a part of the tool
  tool.min_hole_size = 0.01;
  return tool;
}

const static std::string PARAM_BASE = "/process_planning_params/";
const static std::string SCAN_PARAM_BASE = "scan_params/";
const static std::string BLEND_PARAM_BASE = "blend_params/";

const static std::string SPINDLE_SPD_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "spindle_speed";

const static std::string APPROACH_SPD_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "approach_speed";
const static std::string BLENDING_SPD_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "blending_speed";
const static std::string RETRACT_SPD_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "retract_speed";
const static std::string TRAVERSE_SPD_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "traverse_speed";
const static std::string Z_ADJUST_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "z_adjust";

const static std::string TOOL_RADIUS_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "tool_radius";
const static std::string TOOL_OVERLAP_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "overlap";
const static std::string DISCRETIZATION_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "discretization";
const static std::string TRAVERSE_HEIGHT_PARAM = PARAM_BASE + BLEND_PARAM_BASE + "traverse_height";

template<typename T>
static void loadOrWarn(ros::NodeHandle& nh, const std::string& key, T& value)
{
  if (!nh.getParam(key, value))
  {
    ROS_ERROR_STREAM("Could not load parameter: " << nh.resolveName(key));
  }
}

std::ostream& operator<<(std::ostream& os, const tool_path_planner::ProcessTool& tool)
{
  os << "Tool:[line_spacing:=" << tool.line_spacing << ", pt_spacing:=" << tool.pt_spacing << "]";
  return os;
}

tool_path_planner::ProcessTool godel_noether::NoetherBlendPathPlanner::loadTool() const
{
  ros::NodeHandle nh;
  tool_path_planner::ProcessTool tool = NoetherPathPlanner::loadTool();

  // Compute line-spacing
  double tool_radius = 0.025;
  double tool_overlap = 0.0;
  loadOrWarn(nh, TOOL_RADIUS_PARAM, tool_radius);
  loadOrWarn(nh, TOOL_OVERLAP_PARAM, tool_overlap);

  double line_spacing = std::max(0.01, tool_radius * 2.0 - tool_overlap);
  tool.line_spacing = line_spacing;

  loadOrWarn(nh, DISCRETIZATION_PARAM, tool.pt_spacing);
  ROS_WARN_STREAM("NOETHER BLEND: " << tool);
  return tool;
}

tool_path_planner::ProcessTool godel_noether::NoetherScanPathPlanner::loadTool() const
{
  ros::NodeHandle nh;
  tool_path_planner::ProcessTool tool = NoetherPathPlanner::loadTool();

  const static std::string SCAN_OVERLAP_PARAM = PARAM_BASE + SCAN_PARAM_BASE + "overlap";
  const static std::string SCAN_WIDTH_PARAM = PARAM_BASE + SCAN_PARAM_BASE + "scan_width";


  // Compute line-spacing
  double scan_width = 0.025;
  double scan_overlap = 0.0;
  loadOrWarn(nh, SCAN_WIDTH_PARAM, scan_width);
  loadOrWarn(nh, SCAN_OVERLAP_PARAM, scan_overlap);

  double line_spacing = std::max(0.01, scan_width - scan_overlap);
  tool.line_spacing = line_spacing;

  loadOrWarn(nh, DISCRETIZATION_PARAM, tool.pt_spacing);
  ROS_WARN_STREAM("NOETHER SCAN: " << tool);
  return tool;
}

PLUGINLIB_EXPORT_CLASS(godel_noether::NoetherPathPlanner, path_planning_plugins_base::PathPlanningBase)

PLUGINLIB_EXPORT_CLASS(godel_noether::NoetherBlendPathPlanner, path_planning_plugins_base::PathPlanningBase)

PLUGINLIB_EXPORT_CLASS(godel_noether::NoetherScanPathPlanner, path_planning_plugins_base::PathPlanningBase)

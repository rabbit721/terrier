#pragma once

#include <map>
#include <vector>
#include <unordered_set>

#include "common/managed_pointer.h"
#include "self_driving/pilot/action/action_defs.h"

#define NULL_ACTION INT32_MAX

namespace noisepage::selfdriving {
class Pilot;
class WorkloadForecast;

namespace pilot {
class AbstractAction;

/**
 * The pilot processes the query trace predictions by executing them and extracting pipeline features
 */
class TreeNode {
 public:
  /**
   * Constructor for tree node
   * @param parent pointer to parent
   * @param current_action action that leads its parent to the current node, root has NULL action
   * @param cost cost of the node when it's first created (as a leaf)
   */
  TreeNode(common::ManagedPointer<TreeNode> parent, action_id_t current_action, uint64_t cost);

  /**
   * Get action that leads its parent to the current node, root has NULL action
   * @return current action
   */
  action_id_t GetCurrentAction() { return current_action_; }

  /**
   * Get depth of the node (root has depth 0)
   * @return node depth
   */
  uint64_t GetDepth() { return depth_; }

  /**
   * Get pointer to parent node
   * @return pointer to parent node
   */
  common::ManagedPointer<TreeNode> GetParent() { return parent_; };

  /**
   * Return the number of children
   * @return number of children
   */
  uint64_t GetNumberOfChildren() { return children_.size(); }

  /**
   * @return cost_
   */
  uint64_t GetCost() { return cost_; }

  uint64_t ComputeCostFromChildren() {
      auto child_sum = 0;
      for (auto &child : children_) {
        child_sum += child->cost_;
      }
      return child_sum / children_.size();
  }

  /**
   * Update number of visits to the current node aka number of traversals in the tree
   * containing the path to current node, and the cost of the node; based on the expansion of a leaf
   * @param num_expansion number of children of the expanded leaf
   * @param leaf_cost previous cost of the leaf
   * @param new_cost new cost of the leaf
   */
  void UpdateCostAndVisits(uint64_t num_expansion, uint64_t leaf_cost, uint64_t new_cost);

  /**
   * Returns if the node is expanded (has any child)
   * @return is_leaf_
   */
  bool IsLeaf() { return is_leaf_; }

  /**
   * Sample child based on cost and number of visits
   * @return pointer to sampled child
   */
  common::ManagedPointer<TreeNode> SampleChild();

  /**
   * @return child with least cost
   */
  common::ManagedPointer<TreeNode> BestChild();

  /**
   * Expand each child of current node and update its cost and num of visits accordingly
   * @param pilot pointer to pilot
   * @param forecast pointer to forecasted workload
   * @param start_segment_index
   * @param end_segment_index
   * @param db_oids
   * @param action_map_
   * @param candidate_actions_
   */
  void ChildrenRollout(common::ManagedPointer<Pilot> pilot,
                       common::ManagedPointer<WorkloadForecast> forecast,
                       uint64_t start_segment_index,
                       uint64_t end_segment_index,
                       const std::vector<std::vector<uint64_t>> &db_oids,
                       const std::map<action_id_t, std::unique_ptr<AbstractAction>> &action_map_,
                       const std::unordered_set<action_id_t> &candidate_actions_);

 private:
  bool is_leaf_;
  uint64_t depth_; // number of edges in path from root
  uint64_t number_of_visits_; // number of leaf in subtree rooted at node
  common::ManagedPointer<TreeNode> parent_;
  std::vector<std::unique_ptr<TreeNode>> children_;
  action_id_t current_action_{INT32_MAX};
  uint64_t cost_{UINT64_MAX};

};
}

}  // namespace noisepage::selfdriving::pilot

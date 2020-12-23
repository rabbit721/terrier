#pragma once

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "catalog/catalog.h"
#include "common/action_context.h"
#include "common/error/exception.h"
#include "common/shared_latch.h"
#include "execution/exec/execution_settings.h"
#include "execution/exec_defs.h"
#include "gflags/gflags.h"
#include "loggers/settings_logger.h"
#include "messenger/messenger.h"
#include "metrics/metrics_thread.h"
#include "optimizer/statistics/stats_storage.h"
#include "parser/expression/constant_value_expression.h"
#include "self_driving/forecast/workload_forecast.h"
#include "self_driving/model_server/model_server_manager.h"
#include "self_driving/modeling/operating_unit.h"
#include "settings/settings_manager.h"
#include "settings/settings_param.h"
#include "transaction/transaction_manager.h"
#include "type/type_id.h"

namespace noisepage::selfdriving {

/**
 * The pilot processes the query trace predictions by executing them and extracting pipeline features
 */
class Pilot {
 protected:
  static constexpr const char *SAVE_PATH = "/../script/model/terrier_ms_trained/mini_model_test.pickle";
  static constexpr const char *BUILD_ABS_PATH = "BUILD_ABS_PATH";

 public:
  /**
   * Constructor for Pilot
   * @param db_main Managed Pointer to db_main
   * @param workload_forecast_interval Interval used in the forecastor
   */
  Pilot(common::ManagedPointer<catalog::Catalog> catalog, common::ManagedPointer<metrics::MetricsThread> metrics_thread,
        common::ManagedPointer<modelserver::ModelServerManager> model_server_manager,
        common::ManagedPointer<settings::SettingsManager> settings_manager,
        common::ManagedPointer<optimizer::StatsStorage> stats_storage,
        common::ManagedPointer<transaction::TransactionManager> txn_manager, uint64_t workload_forecast_interval);

  /**
   * WorkloadForecast object performing the query execution and feature gathering
   */
  std::unique_ptr<selfdriving::WorkloadForecast> forecast_;

  /**
   * Empty Setter Callback for setting bool value for flags
   */
  static void EmptySetterCallback(common::ManagedPointer<common::ActionContext> action_context UNUSED_ATTRIBUTE) {}

  /**
   * Performs Pilot Logic, load and execute the predict queries while extracting pipeline features
   */
  void PerformPlanning();

 private:
  void ExecuteForecast();
  common::ManagedPointer<catalog::Catalog> catalog_;
  common::ManagedPointer<metrics::MetricsThread> metrics_thread_;
  common::ManagedPointer<modelserver::ModelServerManager> ms_manager_;
  common::ManagedPointer<settings::SettingsManager> settings_manager_;
  common::ManagedPointer<optimizer::StatsStorage> stats_storage_;
  common::ManagedPointer<transaction::TransactionManager> txn_manager_;
  uint64_t workload_forecast_interval_{10000000};
  friend class noisepage::selfdriving::PilotUtil;
};

}  // namespace noisepage::selfdriving

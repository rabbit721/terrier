#include "catalog/postgres/pg_proc_impl.h"

#include <vector>

#include "catalog/database_catalog.h"
#include "catalog/postgres/builder.h"
#include "catalog/postgres/pg_namespace.h"
#include "catalog/postgres/pg_proc.h"
#include "catalog/schema.h"
#include "common/error/error_code.h"
#include "common/error/exception.h"
#include "execution/functions/function_context.h"
#include "storage/index/index.h"
#include "storage/sql_table.h"
#include "transaction/deferred_action_manager.h"

namespace noisepage::catalog::postgres {

PgProcImpl::PgProcImpl(db_oid_t db_oid) : db_oid_(db_oid) {}

void PgProcImpl::BootstrapPRIs() {
  const std::vector<col_oid_t> pg_proc_all_oids{PgProc::PG_PRO_ALL_COL_OIDS.cbegin(),
                                                PgProc::PG_PRO_ALL_COL_OIDS.cend()};
  pg_proc_all_cols_pri_ = procs_->InitializerForProjectedRow(pg_proc_all_oids);
  pg_proc_all_cols_prm_ = procs_->ProjectionMapForOids(pg_proc_all_oids);

  const std::vector<col_oid_t> set_pg_proc_ptr_oids{PgProc::PRO_CTX_PTR_COL_OID};
  pg_proc_ptr_pri_ = procs_->InitializerForProjectedRow(set_pg_proc_ptr_oids);
}

void PgProcImpl::Bootstrap(common::ManagedPointer<transaction::TransactionContext> txn,
                           common::ManagedPointer<DatabaseCatalog> dbc) {
  UNUSED_ATTRIBUTE bool retval;

  retval = dbc->CreateTableEntry(txn, postgres::PgProc::PRO_TABLE_OID, postgres::NAMESPACE_CATALOG_NAMESPACE_OID,
                                 "pg_proc", postgres::Builder::GetProcTableSchema());
  NOISEPAGE_ASSERT(retval, "Bootstrap operations should not fail");
  retval = dbc->SetTablePointer(txn, postgres::PgProc::PRO_TABLE_OID, procs_);
  NOISEPAGE_ASSERT(retval, "Bootstrap operations should not fail");

  retval = dbc->CreateIndexEntry(txn, postgres::NAMESPACE_CATALOG_NAMESPACE_OID, postgres::PgProc::PRO_TABLE_OID,
                                 postgres::PgProc::PRO_OID_INDEX_OID, "pg_proc_oid_index",
                                 postgres::Builder::GetProcOidIndexSchema(db_oid_));
  NOISEPAGE_ASSERT(retval, "Bootstrap operations should not fail");
  retval = dbc->SetIndexPointer(txn, postgres::PgProc::PRO_OID_INDEX_OID, procs_oid_index_);
  NOISEPAGE_ASSERT(retval, "Bootstrap operations should not fail");

  retval = dbc->CreateIndexEntry(txn, postgres::NAMESPACE_CATALOG_NAMESPACE_OID, postgres::PgProc::PRO_TABLE_OID,
                                 postgres::PgProc::PRO_NAME_INDEX_OID, "pg_proc_name_index",
                                 postgres::Builder::GetProcNameIndexSchema(db_oid_));
  NOISEPAGE_ASSERT(retval, "Bootstrap operations should not fail");
  retval = dbc->SetIndexPointer(txn, postgres::PgProc::PRO_NAME_INDEX_OID, procs_name_index_);
  NOISEPAGE_ASSERT(retval, "Bootstrap operations should not fail");

  BootstrapProcs(txn, dbc);
}

std::function<void(void)> PgProcImpl::GetTearDownFn(common::ManagedPointer<transaction::TransactionContext> txn) {
  std::vector<execution::functions::FunctionContext *> func_contexts;

  const std::vector<col_oid_t> pg_proc_contexts{PgProc::PRO_CTX_PTR_COL_OID};
  const auto pci = procs_->InitializerForProjectedColumns(pg_proc_contexts, DatabaseCatalog::TEARDOWN_MAX_TUPLES);
  byte *buffer = common::AllocationUtil::AllocateAligned(pci.ProjectedColumnsSize());
  auto pc = pci.Initialize(buffer);

  auto ctxs = reinterpret_cast<execution::functions::FunctionContext **>(pc->ColumnStart(0));

  // Collect all the non-nullptr contexts in pg_proc.
  auto table_iter = procs_->begin();
  while (table_iter != procs_->end()) {
    procs_->Scan(txn, &table_iter, pc);

    for (uint i = 0; i < pc->NumTuples(); i++) {
      if (ctxs[i] == nullptr) {
        continue;
      }
      func_contexts.emplace_back(ctxs[i]);
    }
  }

  delete[] buffer;
  return [func_contexts{std::move(func_contexts)}]() {
    for (auto ctx : func_contexts) {
      delete ctx;
    }
  };
}

bool PgProcImpl::CreateProcedure(const common::ManagedPointer<transaction::TransactionContext> txn,
                                 const proc_oid_t oid, const std::string &procname, const language_oid_t language_oid,
                                 const namespace_oid_t procns, const std::vector<std::string> &args,
                                 const std::vector<type_oid_t> &arg_types, const std::vector<type_oid_t> &all_arg_types,
                                 const std::vector<PgProc::ArgModes> &arg_modes, const type_oid_t rettype,
                                 const std::string &src, const bool is_aggregate) {
  NOISEPAGE_ASSERT(args.size() < UINT16_MAX, "Number of arguments must fit in a SMALLINT");

  const auto name_varlen = storage::StorageUtil::CreateVarlen(procname);

  auto *const redo = txn->StageWrite(db_oid_, PgProc::PRO_TABLE_OID, pg_proc_all_cols_pri_);
  auto *delta = redo->Delta();
  auto &pm = pg_proc_all_cols_prm_;

  // Prepare the PR for insertion.
  {
    std::vector<std::string> arg_name_vec;
    arg_name_vec.reserve(args.size() * sizeof(storage::VarlenEntry));
    for (auto &arg : args) {
      arg_name_vec.push_back(arg);
    }

    const auto arg_names_varlen = storage::StorageUtil::CreateVarlen(args);
    const auto arg_types_varlen = storage::StorageUtil::CreateVarlen(arg_types);
    const auto all_arg_types_varlen = storage::StorageUtil::CreateVarlen(all_arg_types);
    const auto arg_modes_varlen = storage::StorageUtil::CreateVarlen(arg_modes);
    const auto src_varlen = storage::StorageUtil::CreateVarlen(src);

    delta->Set<proc_oid_t, false>(pm[PgProc::PROOID_COL_OID], oid, false);                     // Procedure OID.
    delta->Set<storage::VarlenEntry, false>(pm[PgProc::PRONAME_COL_OID], name_varlen, false);  // Procedure name.
    delta->Set<namespace_oid_t, false>(pm[PgProc::PRONAMESPACE_COL_OID], procns, false);  // Namespace of procedure.
    delta->Set<language_oid_t, false>(pm[PgProc::PROLANG_COL_OID], language_oid, false);  // Language for procedure.
    delta->Set<double, false>(pm[PgProc::PROCOST_COL_OID], 0, false);  // Estimated cost per row returned.
    delta->Set<double, false>(pm[PgProc::PROROWS_COL_OID], 0, false);  // Estimated number of result rows.
    // The Postgres documentation says that provariadic should be 0 if no variadics are present.
    // Otherwise, it is the data type of the variadic array parameter's elements.
    // TODO(WAN): Hang on, how are we using CreateProcedure for variadics then?
    delta->Set<type_oid_t, false>(pm[PgProc::PROVARIADIC_COL_OID], type_oid_t{0}, false);  // Assume no variadics.
    delta->Set<bool, false>(pm[PgProc::PROISAGG_COL_OID], is_aggregate, false);            // Whether aggregate or not.
    delta->Set<bool, false>(pm[PgProc::PROISWINDOW_COL_OID], false, false);                // Not window.
    delta->Set<bool, false>(pm[PgProc::PROISSTRICT_COL_OID], true, false);                 // Strict.
    delta->Set<bool, false>(pm[PgProc::PRORETSET_COL_OID], false, false);                  // Doesn't return a set.
    delta->Set<char, false>(pm[PgProc::PROVOLATILE_COL_OID], static_cast<char>(PgProc::ProVolatile::STABLE),
                            false);                                                                        // Stable.
    delta->Set<uint16_t, false>(pm[PgProc::PRONARGS_COL_OID], static_cast<uint16_t>(args.size()), false);  // Num args.
    delta->Set<uint16_t, false>(pm[PgProc::PRONARGDEFAULTS_COL_OID], 0, false);     // Assume no default args.
    delta->Set<type_oid_t, false>(pm[PgProc::PRORETTYPE_COL_OID], rettype, false);  // Return type.
    delta->Set<storage::VarlenEntry, false>(pm[PgProc::PROARGTYPES_COL_OID], arg_types_varlen, false);  // Arg types.
    // TODO(WAN): proallargtypes and proargmodes in Postgres should be NULL most of the time. See #1359.
    delta->Set<storage::VarlenEntry, false>(pm[PgProc::PROALLARGTYPES_COL_OID], all_arg_types_varlen, false);
    delta->Set<storage::VarlenEntry, false>(pm[PgProc::PROARGMODES_COL_OID], arg_modes_varlen, false);
    delta->SetNull(pm[PgProc::PROARGDEFAULTS_COL_OID]);  // Assume no default args.
    delta->Set<storage::VarlenEntry, false>(pm[PgProc::PROARGNAMES_COL_OID], arg_names_varlen, false);  // Arg names.
    delta->Set<storage::VarlenEntry, false>(pm[PgProc::PROSRC_COL_OID], src_varlen, false);             // Source code.
    delta->SetNull(pm[PgProc::PROCONFIG_COL_OID]);    // Assume no procedure local run-time configuration.
    delta->SetNull(pm[PgProc::PRO_CTX_PTR_COL_OID]);  // Pointer to procedure context.
  }

  const auto tuple_slot = procs_->Insert(txn, redo);

  auto oid_pri = procs_oid_index_->GetProjectedRowInitializer();
  auto name_pri = procs_name_index_->GetProjectedRowInitializer();
  byte *const buffer = common::AllocationUtil::AllocateAligned(name_pri.ProjectedRowSize());

  // Insert into pg_proc_name_index.
  {
    auto name_pr = name_pri.InitializeRow(buffer);
    auto name_map = procs_name_index_->GetKeyOidToOffsetMap();
    name_pr->Set<namespace_oid_t, false>(name_map[indexkeycol_oid_t(1)], procns, false);
    name_pr->Set<storage::VarlenEntry, false>(name_map[indexkeycol_oid_t(2)], name_varlen, false);

    if (auto result = procs_name_index_->Insert(txn, *name_pr, tuple_slot); !result) {
      delete[] buffer;
      return false;
    }
  }

  // Insert into pg_proc_oid_index.
  {
    auto oid_pr = oid_pri.InitializeRow(buffer);
    oid_pr->Set<proc_oid_t, false>(0, oid, false);
    bool UNUSED_ATTRIBUTE result = procs_oid_index_->InsertUnique(txn, *oid_pr, tuple_slot);
    NOISEPAGE_ASSERT(result, "Oid insertion should be unique");
  }

  delete[] buffer;
  return true;
}

bool PgProcImpl::DropProcedure(const common::ManagedPointer<transaction::TransactionContext> txn, proc_oid_t proc) {
  NOISEPAGE_ASSERT(proc != INVALID_PROC_OID, "Invalid oid passed");

  auto name_pri = procs_name_index_->GetProjectedRowInitializer();
  auto oid_pri = procs_oid_index_->GetProjectedRowInitializer();
  byte *const buffer = common::AllocationUtil::AllocateAligned(pg_proc_all_cols_pri_.ProjectedRowSize());

  auto oid_pr = oid_pri.InitializeRow(buffer);
  oid_pr->Set<proc_oid_t, false>(0, proc, false);

  // Look for the procedure in pg_proc_oid_index.
  std::vector<storage::TupleSlot> results;
  {
    procs_oid_index_->ScanKey(*txn, *oid_pr, &results);
    if (results.empty()) {
      delete[] buffer;
      return false;
    }
  }

  NOISEPAGE_ASSERT(results.size() == 1, "More than one non-unique result found in unique index.");

  auto to_delete_slot = results[0];

  // Delete from pg_proc.
  {
    txn->StageDelete(db_oid_, LANGUAGE_TABLE_OID, to_delete_slot);
    if (!procs_->Delete(txn, to_delete_slot)) {
      // Someone else has a write-lock. Free the buffer and return false to indicate failure.
      delete[] buffer;
      return false;
    }
  }

  // Delete from pg_proc_oid_index. Reuses oid_pr from the index scan above.
  { procs_oid_index_->Delete(txn, *oid_pr, to_delete_slot); }

  // Look for the procedure in pg_proc.
  auto table_pr = pg_proc_all_cols_pri_.InitializeRow(buffer);
  bool UNUSED_ATTRIBUTE visible = procs_->Select(txn, to_delete_slot, table_pr);

  auto &proc_pm = pg_proc_all_cols_prm_;
  auto name_varlen = *table_pr->Get<storage::VarlenEntry, false>(proc_pm[PgProc::PRONAME_COL_OID], nullptr);
  auto proc_ns = *table_pr->Get<namespace_oid_t, false>(proc_pm[PgProc::PRONAMESPACE_COL_OID], nullptr);
  auto ctx_ptr = table_pr->AccessWithNullCheck(proc_pm[PgProc::PRO_CTX_PTR_COL_OID]);

  // Delete from pg_proc_name_index.
  {
    auto name_pr = name_pri.InitializeRow(buffer);
    auto name_map = procs_name_index_->GetKeyOidToOffsetMap();
    name_pr->Set<namespace_oid_t, false>(name_map[indexkeycol_oid_t(1)], proc_ns, false);
    name_pr->Set<storage::VarlenEntry, false>(name_map[indexkeycol_oid_t(2)], name_varlen, false);
    procs_name_index_->Delete(txn, *name_pr, to_delete_slot);
  }

  // Clean up the procedure context.
  if (ctx_ptr != nullptr) {
    txn->RegisterCommitAction([=](transaction::DeferredActionManager *deferred_action_manager) {
      deferred_action_manager->RegisterDeferredAction(
          [=]() { deferred_action_manager->RegisterDeferredAction([=]() { delete ctx_ptr; }); });
    });
  }

  delete[] buffer;
  return true;
}

bool PgProcImpl::SetProcCtxPtr(common::ManagedPointer<transaction::TransactionContext> txn, const proc_oid_t proc_oid,
                               const execution::functions::FunctionContext *func_context) {
  auto oid_pri = procs_oid_index_->GetProjectedRowInitializer();
  auto *const index_buffer = common::AllocationUtil::AllocateAligned(oid_pri.ProjectedRowSize());

  // Look for the procedure in pg_proc_oid_index.
  std::vector<storage::TupleSlot> index_results;
  {
    auto *const key_pr = oid_pri.InitializeRow(index_buffer);
    key_pr->Set<proc_oid_t, false>(0, proc_oid, false);
    procs_oid_index_->ScanKey(*txn, *key_pr, &index_results);
    NOISEPAGE_ASSERT(
        index_results.size() == 1,
        "Incorrect number of results from index scan. Expect 1 because it's a unique index. 0 implies that function "
        "was called with an oid that doesn't exist in the Catalog, which implies a programmer error. There's no "
        "reasonable code path for this to be called on an oid that isn't present.");
  }

  delete[] index_buffer;

  // Update pg_proc.
  auto *const update_redo = txn->StageWrite(db_oid_, PgProc::PRO_TABLE_OID, pg_proc_ptr_pri_);
  update_redo->Delta()->Set<const execution::functions::FunctionContext *, false>(0, func_context, false);
  update_redo->SetTupleSlot(index_results[0]);
  return procs_->Update(txn, update_redo);
}

common::ManagedPointer<execution::functions::FunctionContext> PgProcImpl::GetProcCtxPtr(
    common::ManagedPointer<transaction::TransactionContext> txn, proc_oid_t proc_oid) {
  auto oid_pri = procs_oid_index_->GetProjectedRowInitializer();
  auto *const buffer = common::AllocationUtil::AllocateAligned(pg_proc_ptr_pri_.ProjectedRowSize());

  // Look for the procedure in pg_proc_oid_index.
  std::vector<storage::TupleSlot> index_results;
  {
    auto *const key_pr = oid_pri.InitializeRow(buffer);
    key_pr->Set<proc_oid_t, false>(0, proc_oid, false);
    procs_oid_index_->ScanKey(*txn, *key_pr, &index_results);
    NOISEPAGE_ASSERT(
        index_results.size() == 1,
        "Incorrect number of results from index scan. Expect 1 because it's a unique index. 0 implies that function "
        "was called with an oid that doesn't exist in the Catalog, which implies a programmer error. There's no"
        "reasonable code path for this to be called on an oid that isn't present.");
  }

  auto *select_pr = pg_proc_ptr_pri_.InitializeRow(buffer);
  const auto result UNUSED_ATTRIBUTE = procs_->Select(txn, index_results[0], select_pr);
  NOISEPAGE_ASSERT(result, "Index already verified visibility. This shouldn't fail.");

  auto *ptr_ptr = (reinterpret_cast<void **>(select_pr->AccessWithNullCheck(0)));
  NOISEPAGE_ASSERT(nullptr != ptr_ptr, "GetProcCtxPtr called on an invalid OID or before SetProcCtxPtr.");
  execution::functions::FunctionContext *ptr = *reinterpret_cast<execution::functions::FunctionContext **>(ptr_ptr);

  delete[] buffer;
  return common::ManagedPointer<execution::functions::FunctionContext>(ptr);
}

proc_oid_t PgProcImpl::GetProcOid(const common::ManagedPointer<transaction::TransactionContext> txn,
                                  const common::ManagedPointer<DatabaseCatalog> dbc, const namespace_oid_t procns,
                                  const std::string &procname, const std::vector<type_oid_t> &arg_types) {
  auto name_pri = procs_name_index_->GetProjectedRowInitializer();
  byte *const buffer = common::AllocationUtil::AllocateAligned(pg_proc_all_cols_pri_.ProjectedRowSize());

  // Look for the procedure in pg_proc_name_index.
  std::vector<storage::TupleSlot> results;
  {
    auto name_pr = name_pri.InitializeRow(buffer);
    auto name_map = procs_name_index_->GetKeyOidToOffsetMap();

    auto name_varlen = storage::StorageUtil::CreateVarlen(procname);
    name_pr->Set<namespace_oid_t, false>(name_map[indexkeycol_oid_t(1)], procns, false);
    name_pr->Set<storage::VarlenEntry, false>(name_map[indexkeycol_oid_t(2)], name_varlen, false);
    procs_name_index_->ScanKey(*txn, *name_pr, &results);

    if (name_varlen.NeedReclaim()) {
      delete[] name_varlen.Content();
    }
  }

  proc_oid_t ret = INVALID_PROC_OID;
  std::vector<proc_oid_t> matching_functions;

  if (!results.empty()) {
    const std::vector<type_oid_t> variadic = {dbc->GetTypeOidForType(type::TypeId::VARIADIC)};
    auto variadic_varlen = storage::StorageUtil::CreateVarlen(variadic);
    auto all_arg_types_varlen = storage::StorageUtil::CreateVarlen(arg_types);

    // Search through the results and check if any match the parsed function by argument types.
    for (auto &tuple : results) {
      auto table_pr = pg_proc_all_cols_pri_.InitializeRow(buffer);
      bool UNUSED_ATTRIBUTE visible = procs_->Select(txn, tuple, table_pr);

      // "PROARGTYPES ... represents the call signature of the function". Check only input arguments.
      // https://www.postgresql.org/docs/12/catalog-pg-proc.html
      auto &pm = pg_proc_all_cols_prm_;
      auto ind_all_arg_types = *table_pr->Get<storage::VarlenEntry, false>(pm[PgProc::PROARGTYPES_COL_OID], nullptr);

      // Variadic functions will match any argument type as long as there are one or more arguments.
      bool matches_exactly = ind_all_arg_types == all_arg_types_varlen;
      bool is_variadic = ind_all_arg_types == variadic_varlen && !arg_types.empty();

      if (matches_exactly || is_variadic) {
        auto proc_oid = *table_pr->Get<proc_oid_t, false>(pm[PgProc::PROOID_COL_OID], nullptr);
        matching_functions.push_back(proc_oid);
        break;
      }
    }
    if (variadic_varlen.NeedReclaim()) {
      delete[] variadic_varlen.Content();
    }
    if (all_arg_types_varlen.NeedReclaim()) {
      delete[] all_arg_types_varlen.Content();
    }
  }

  delete[] buffer;

  if (matching_functions.size() == 1) {
    ret = matching_functions[0];
  } else if (matching_functions.size() > 1) {
    // TODO(WAN): See #1361 for a discussion on whether we can avoid this
    // TODO(Joe Koshakow) would be nice to to include the parsed arg types of the function and the arg types that it
    // matches with
    throw BINDER_EXCEPTION(
        fmt::format(
            "Ambiguous function \"{}\", with given types. It matches multiple function signatures in the catalog",
            procname),
        common::ErrorCode::ERRCODE_DUPLICATE_FUNCTION);
  }

  return ret;
}

void PgProcImpl::BootstrapProcs(const common::ManagedPointer<transaction::TransactionContext> txn,
                                const common::ManagedPointer<DatabaseCatalog> dbc) {
  const auto DEC = dbc->GetTypeOidForType(type::TypeId::DECIMAL);   // NOLINT
  const auto INT = dbc->GetTypeOidForType(type::TypeId::INTEGER);   // NOLINT
  const auto STR = dbc->GetTypeOidForType(type::TypeId::VARCHAR);   // NOLINT
  const auto REAL = dbc->GetTypeOidForType(type::TypeId::DECIMAL);  // NOLINT
  const auto DATE = dbc->GetTypeOidForType(type::TypeId::DATE);     // NOLINT
  const auto BOOL = dbc->GetTypeOidForType(type::TypeId::BOOLEAN);  // NOLINT
  const auto VAR = dbc->GetTypeOidForType(type::TypeId::VARIADIC);  // NOLINT

  auto create_fn = [&](const std::string &procname, const std::vector<std::string> &args,
                       const std::vector<type_oid_t> &arg_types, const std::vector<type_oid_t> &all_arg_types,
                       type_oid_t rettype, bool is_aggregate) {
    CreateProcedure(txn, proc_oid_t{dbc->next_oid_++}, procname, INTERNAL_LANGUAGE_OID, NAMESPACE_DEFAULT_NAMESPACE_OID,
                    args, arg_types, all_arg_types, {}, rettype, "", true);
  };

  // Math functions.
  create_fn("abs", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("abs", {"n"}, {INT}, {INT}, INT, true);
  create_fn("ceil", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("cbrt", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("exp", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("floor", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("log10", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("log2", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("mod", {"a", "b"}, {DEC, DEC}, {DEC, DEC}, DEC, true);
  create_fn("mod", {"a", "b"}, {INT, INT}, {INT, INT}, INT, true);
  create_fn("pow", {"x", "y"}, {DEC, DEC}, {DEC, DEC}, DEC, true);
  create_fn("round", {"x", "n"}, {DEC, INT}, {DEC, INT}, DEC, true);
  create_fn("round", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("sqrt", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("truncate", {"x"}, {DEC}, {DEC}, DEC, true);

  // Trig functions.
  create_fn("acos", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("asin", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("atan", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("atan2", {"y", "x"}, {DEC, DEC}, {DEC, DEC}, DEC, true);
  create_fn("cos", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("cosh", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("cot", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("sin", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("sinh", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("tan", {"x"}, {DEC}, {DEC}, DEC, true);
  create_fn("tanh", {"x"}, {DEC}, {DEC}, DEC, true);

  // String functions.
  create_fn("ascii", {"s"}, {STR}, {STR}, INT, true);
  create_fn("btrim", {"s"}, {STR}, {STR}, STR, true);
  create_fn("btrim", {"s", "s"}, {STR, STR}, {STR, STR}, STR, true);
  create_fn("char_length", {"s"}, {STR}, {STR}, INT, true);
  create_fn("chr", {"n"}, {INT}, {INT}, STR, true);
  create_fn("concat", {"s"}, {VAR}, {VAR}, STR, true);
  create_fn("initcap", {"s"}, {STR}, {STR}, STR, true);
  create_fn("lower", {"s"}, {STR}, {STR}, STR, true);
  create_fn("left", {"s", "n"}, {STR, INT}, {STR, INT}, STR, true);
  create_fn("length", {"s"}, {STR}, {STR}, INT, true);
  create_fn("lpad", {"s", "len"}, {STR, INT}, {STR, INT}, STR, true);
  create_fn("lpad", {"s", "len", "pad"}, {STR, INT, STR}, {STR, INT, STR}, STR, true);
  create_fn("ltrim", {"s"}, {STR}, {STR}, STR, true);
  create_fn("ltrim", {"s", "chars"}, {STR, STR}, {STR, STR}, STR, true);
  create_fn("position", {"s1", "s2"}, {STR, STR}, {STR, STR}, INT, true);
  create_fn("repeat", {"s", "n"}, {STR, INT}, {STR, INT}, STR, true);
  create_fn("reverse", {"s"}, {STR}, {STR}, STR, true);
  create_fn("right", {"s", "n"}, {STR, INT}, {STR, INT}, STR, true);
  create_fn("rpad", {"s", "len"}, {STR, INT}, {STR, INT}, STR, true);
  create_fn("rpad", {"s", "len", "pad"}, {STR, INT, STR}, {STR, INT, STR}, STR, true);
  create_fn("rtrim", {"s"}, {STR}, {STR}, STR, true);
  create_fn("rtrim", {"s", "chars"}, {STR, STR}, {STR, STR}, STR, true);
  create_fn("split_part", {"s", "delim", "field"}, {STR, STR, INT}, {STR, STR, INT}, STR, true);
  create_fn("starts_with", {"s", "start"}, {STR, STR}, {STR, STR}, BOOL, true);
  create_fn("substr", {"s", "pos", "len"}, {STR, INT, INT}, {STR, INT, INT}, STR, true);
  create_fn("upper", {"s"}, {STR}, {STR}, STR, true);

  // Other functions.
  create_fn("date_part", {"date, date_part_type"}, {DATE, INT}, {DATE, INT}, INT, false);
  create_fn("version", {}, {}, {}, STR, false);

  CreateProcedure(
      txn, proc_oid_t{dbc->next_oid_++}, "nprunnersemitint", INTERNAL_LANGUAGE_OID, NAMESPACE_DEFAULT_NAMESPACE_OID,
      {"num_tuples", "num_cols", "num_int_cols", "num_real_cols"}, {INT, INT, INT, INT}, {INT, INT, INT, INT},
      {PgProc::ArgModes::IN, PgProc::ArgModes::IN, PgProc::ArgModes::IN, PgProc::ArgModes::IN}, INT, "", false);
  CreateProcedure(
      txn, proc_oid_t{dbc->next_oid_++}, "nprunnersemitreal", INTERNAL_LANGUAGE_OID, NAMESPACE_DEFAULT_NAMESPACE_OID,
      {"num_tuples", "num_cols", "num_int_cols", "num_real_cols"}, {INT, INT, INT, INT}, {INT, INT, INT, INT},
      {PgProc::ArgModes::IN, PgProc::ArgModes::IN, PgProc::ArgModes::IN, PgProc::ArgModes::IN}, REAL, "", false);
  CreateProcedure(txn, proc_oid_t{dbc->next_oid_++}, "nprunnersdummyint", INTERNAL_LANGUAGE_OID,
                  NAMESPACE_DEFAULT_NAMESPACE_OID, {}, {}, {}, {}, INT, "", false);
  CreateProcedure(txn, proc_oid_t{dbc->next_oid_++}, "nprunnersdummyreal", INTERNAL_LANGUAGE_OID,
                  NAMESPACE_DEFAULT_NAMESPACE_OID, {}, {}, {}, {}, REAL, "", false);

  BootstrapProcContexts(txn, dbc);
}

void PgProcImpl::BootstrapProcContext(const common::ManagedPointer<transaction::TransactionContext> txn,
                                      const common::ManagedPointer<DatabaseCatalog> dbc, std::string &&func_name,
                                      const type::TypeId func_ret_type, std::vector<type::TypeId> &&arg_types,
                                      const execution::ast::Builtin builtin, const bool is_exec_ctx_required) {
  std::vector<type_oid_t> arg_type_oids;
  arg_type_oids.reserve(arg_types.size());
  for (const auto type : arg_types) {
    arg_type_oids.emplace_back(dbc->GetTypeOidForType(type));
  }
  const proc_oid_t proc_oid = GetProcOid(txn, dbc, NAMESPACE_DEFAULT_NAMESPACE_OID, func_name, arg_type_oids);
  const auto *const func_context = new execution::functions::FunctionContext(
      std::move(func_name), func_ret_type, std::move(arg_types), builtin, is_exec_ctx_required);
  const auto retval UNUSED_ATTRIBUTE = dbc->SetProcCtxPtr(txn, proc_oid, func_context);
  NOISEPAGE_ASSERT(retval, "Bootstrap operations should not fail");
}

void PgProcImpl::BootstrapProcContexts(const common::ManagedPointer<transaction::TransactionContext> txn,
                                       const common::ManagedPointer<DatabaseCatalog> dbc) {
  auto DEC = type::TypeId::DECIMAL;  // NOLINT
  auto INT = type::TypeId::INTEGER;  // NOLINT
  auto VAR = type::TypeId::VARCHAR;  // NOLINT

  auto create_fn = [&](std::string &&func_name, type::TypeId func_ret_type, std::vector<type::TypeId> &&arg_types,
                       execution::ast::Builtin builtin, bool is_exec_ctx_required) {
    BootstrapProcContext(txn, dbc, std::move(func_name), func_ret_type, std::move(arg_types), builtin,
                         is_exec_ctx_required);
  };

  // Math functions.
  create_fn("abs", DEC, {DEC}, execution::ast::Builtin::Abs, false);
  create_fn("abs", INT, {INT}, execution::ast::Builtin::Abs, false);
  create_fn("ceil", DEC, {DEC}, execution::ast::Builtin::Ceil, false);
  create_fn("cbrt", DEC, {DEC}, execution::ast::Builtin::Cbrt, false);
  create_fn("exp", DEC, {DEC}, execution::ast::Builtin::Exp, true);
  create_fn("floor", DEC, {DEC}, execution::ast::Builtin::Floor, false);
  create_fn("log10", DEC, {DEC}, execution::ast::Builtin::Log10, false);
  create_fn("log2", DEC, {DEC}, execution::ast::Builtin::Log2, false);
  create_fn("mod", DEC, {DEC, DEC}, execution::ast::Builtin::Mod, false);
  create_fn("mod", INT, {INT, INT}, execution::ast::Builtin::Mod, false);
  create_fn("pow", DEC, {DEC, DEC}, execution::ast::Builtin::Pow, false);
  create_fn("round", DEC, {DEC}, execution::ast::Builtin::Round, false);
  create_fn("round", DEC, {DEC, INT}, execution::ast::Builtin::Round2, false);
  create_fn("sqrt", DEC, {DEC}, execution::ast::Builtin::Sqrt, false);
  create_fn("truncate", DEC, {DEC}, execution::ast::Builtin::Truncate, false);

  // Trig functions.
  create_fn("acos", DEC, {DEC}, execution::ast::Builtin::ACos, false);
  create_fn("asin", DEC, {DEC}, execution::ast::Builtin::ASin, false);
  create_fn("atan", DEC, {DEC}, execution::ast::Builtin::ATan, false);
  create_fn("atan2", DEC, {DEC, DEC}, execution::ast::Builtin::ATan2, false);
  create_fn("cos", DEC, {DEC}, execution::ast::Builtin::Cos, false);
  create_fn("cosh", DEC, {DEC}, execution::ast::Builtin::Cosh, false);
  create_fn("cot", DEC, {DEC}, execution::ast::Builtin::Cot, false);
  create_fn("sin", DEC, {DEC}, execution::ast::Builtin::Sin, false);
  create_fn("sinh", DEC, {DEC}, execution::ast::Builtin::Sinh, false);
  create_fn("tan", DEC, {DEC}, execution::ast::Builtin::Tan, false);
  create_fn("tanh", DEC, {DEC}, execution::ast::Builtin::Tanh, false);

  // String functions.
  create_fn("ascii", INT, {VAR}, execution::ast::Builtin::ASCII, true);
  create_fn("btrim", VAR, {VAR}, execution::ast::Builtin::Trim, true);
  create_fn("btrim", VAR, {VAR, VAR}, execution::ast::Builtin::Trim2, true);
  create_fn("concat", VAR, {VAR}, execution::ast::Builtin::Concat, true);
  create_fn("char_length", INT, {VAR}, execution::ast::Builtin::CharLength, true);
  create_fn("chr", VAR, {INT}, execution::ast::Builtin::Chr, true);
  create_fn("initcap", VAR, {VAR}, execution::ast::Builtin::InitCap, true);
  create_fn("lower", VAR, {VAR}, execution::ast::Builtin::Lower, true);
  create_fn("left", VAR, {VAR, INT}, execution::ast::Builtin::Left, true);
  create_fn("length", INT, {VAR}, execution::ast::Builtin::Length, true);
  create_fn("lpad", VAR, {VAR, INT, VAR}, execution::ast::Builtin::Lpad, true);
  create_fn("lpad", VAR, {VAR, INT}, execution::ast::Builtin::Lpad, true);
  create_fn("ltrim", VAR, {VAR, VAR}, execution::ast::Builtin::Ltrim, true);
  create_fn("ltrim", VAR, {VAR}, execution::ast::Builtin::Ltrim, true);
  create_fn("position", INT, {VAR, VAR}, execution::ast::Builtin::Position, true);
  create_fn("repeat", VAR, {VAR, INT}, execution::ast::Builtin::Repeat, true);
  create_fn("right", VAR, {VAR, INT}, execution::ast::Builtin::Right, true);
  create_fn("reverse", VAR, {VAR}, execution::ast::Builtin::Reverse, true);
  create_fn("rpad", VAR, {VAR, INT, VAR}, execution::ast::Builtin::Rpad, true);
  create_fn("rpad", VAR, {VAR, INT}, execution::ast::Builtin::Rpad, true);
  create_fn("rtrim", VAR, {VAR, VAR}, execution::ast::Builtin::Rtrim, true);
  create_fn("rtrim", VAR, {VAR}, execution::ast::Builtin::Rtrim, true);
  create_fn("split_part", VAR, {VAR, VAR, INT}, execution::ast::Builtin::SplitPart, true);
  create_fn("starts_with", type::TypeId::BOOLEAN, {VAR, VAR}, execution::ast::Builtin::StartsWith, true);
  create_fn("substr", VAR, {VAR, INT, INT}, execution::ast::Builtin::Substring, true);
  create_fn("upper", VAR, {VAR}, execution::ast::Builtin::Upper, true);

  // Other functions.
  create_fn("date_part", INT, {type::TypeId::DATE, INT}, execution::ast::Builtin::DatePart, false);
  create_fn("version", VAR, {}, execution::ast::Builtin::Version, true);

  create_fn("nprunnersemitint", INT, {INT, INT, INT, INT}, execution::ast::Builtin::NpRunnersEmitInt, true);
  create_fn("nprunnersemitreal", DEC, {INT, INT, INT, INT}, execution::ast::Builtin::NpRunnersEmitReal, true);
  create_fn("nprunnersdummyint", INT, {}, execution::ast::Builtin::NpRunnersDummyInt, true);
  create_fn("nprunnersdummyreal", DEC, {}, execution::ast::Builtin::NpRunnersDummyReal, true);
}

}  // namespace noisepage::catalog::postgres

/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "src/util/instruction_counter.h"
#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/main/test.h"

namespace test_harness {
/* Defines what data is written to the tracking table for use in custom validation. */
class operation_tracker_session_microbenchmarks : public operation_tracker {

public:
    operation_tracker_session_microbenchmarks(
      configuration *config, const bool use_compression, timestamp_manager &tsm)
        : operation_tracker(config, use_compression, tsm)
    {
    }

    void
    set_tracking_cursor(WT_SESSION *session, const tracking_operation &operation,
      const uint64_t &collection_id, const std::string &key, const std::string &value,
      wt_timestamp_t ts, scoped_cursor &op_track_cursor) override final
    {
        /* You can replace this call to define your own tracking table contents. */
        operation_tracker::set_tracking_cursor(
          session, operation, collection_id, key, value, ts, op_track_cursor);
    }
};

static void
make_insert(thread_worker *tc, const std::string &id)
{
    std::string cursor_uri = tc->db.get_collection(0).name;

    auto cursor = tc->session.open_scoped_cursor(cursor_uri);
    cursor->set_key(cursor.get(), "key" + id);
    cursor->set_value(cursor.get(), "value1" + id);
    int ret = cursor->insert(cursor.get());

    if (ret != 0) {
        logger::log_msg(
          LOG_ERROR, "session_microbenchmarks: expected insert id " + id + " to succeed");
    }
}

/*
 * Class that defines operations that do nothing as an example. This shows how database operations
 * can be overridden and customized.
 */
class session_microbenchmarks : public test {
public:
    session_microbenchmarks(const test_args &args) : test(args)
    {
        init_operation_tracker(
          new operation_tracker_session_microbenchmarks(_config->get_subconfig(OPERATION_TRACKER),
            _config->get_bool(COMPRESSION_ENABLED), *_timestamp_manager));
    }

    void
    run() override final
    {
        /* You can remove the call to the base class to fully customize your test. */
        test::run();
    }

    void
    background_compact_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "background_compact_operation: nothing done");
    }

    void
    checkpoint_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "checkpoint_operation: nothing done");
    }

    void
    custom_operation(thread_worker *tc) override final
    {
        WT_SESSION wt_session = *(tc->session);
        instruction_counter begin_transaction_counter(
          "begin_transaction_instructions", test::_args.test_name);
        instruction_counter commit_transaction_counter(
          "commit_transaction_instructions", test::_args.test_name);
        instruction_counter rollback_trancation_counter(
          "rollback_trancation_instructions", test::_args.test_name);
        instruction_counter prepare_transaction_counter(
          "prepare_transaction_instructions", test::_args.test_name);

        instruction_counter commit_after_prepare_transaction_counter(
          "commit_after_prepare_instructions", test::_args.test_name);
        instruction_counter open_cursor_counter("open_cursor_instructions", test::_args.test_name);
        instruction_counter timestamp_transaction_uint_counter(
          "timestamp_transaction_uint_instructions", test::_args.test_name);
        std::string cursor_uri = tc->db.get_collection(0).name;

        /* Get the collection to work on. */
        testutil_assert(tc->collection_count == 1);

        scoped_session &session = tc->session;

        begin_transaction_counter.track([&wt_session, &session]() -> int {
            session->begin_transaction(session.get(), NULL);
            return 0;
        });

        make_insert(tc, "1");

        commit_transaction_counter.track([&wt_session, &session]() -> int {
            session->commit_transaction(session.get(), NULL);
            return 0;
        });

        session->begin_transaction(session.get(), NULL);

        make_insert(tc, "2");

        rollback_trancation_counter.track([&wt_session, &session]() -> int {
            session->rollback_transaction(session.get(), NULL);
            return 0;
        });

        std::string prepare_timestamp = tc->tsm->decimal_to_hex(tc->tsm->get_next_ts());
        session->begin_transaction(session.get(), NULL);
        make_insert(tc, "3");
        prepare_transaction_counter.track([&session, &prepare_timestamp]() -> int {
            session->prepare_transaction(
              session.get(), ("prepare_timestamp=" + prepare_timestamp).c_str());
            return 0;
        });

        std::string commit_timestamp = tc->tsm->decimal_to_hex(tc->tsm->get_next_ts());

        commit_after_prepare_transaction_counter.track([&session, &commit_timestamp]() -> int {
            session->commit_transaction(session.get(),
              ("commit_timestamp=" + commit_timestamp + ",durable_timestamp=" + commit_timestamp)
                .c_str());
            return 0;
        });

        WT_CURSOR *cursorp = NULL;
        open_cursor_counter.track([&session, &cursor_uri, &cursorp]() -> int {
            session->open_cursor(session.get(), cursor_uri.c_str(), NULL, NULL, &cursorp);
            return 0;
        });
        cursorp->close(cursorp);

        session->begin_transaction(session.get(), NULL);
        make_insert(tc, "4");
        auto timestamp = tc->tsm->get_next_ts();

        timestamp_transaction_uint_counter.track([&session, &timestamp]() -> int {
            session->timestamp_transaction_uint(session.get(), WT_TS_TXN_TYPE_COMMIT, timestamp);
            return 0;
        });
        session->rollback_transaction(session.get(), NULL);
    }

    void
    insert_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "insert_operation: nothing done");
    }

    void
    read_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "read_operation: nothing done");
    }

    void
    remove_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "remove_operation: nothing done");
    }

    void
    update_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "update_operation: nothing done");
    }

    void
    validate(bool, const std::string &, const std::string &, database &) override final
    {
        logger::log_msg(LOG_WARN, "validate: nothing done");
    }
};

} // namespace test_harness

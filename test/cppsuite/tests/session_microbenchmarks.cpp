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
        init_operation_tracker(NULL);
    }

    void
    checkpoint_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "checkpoint_operation: not done as this is a performance test");
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
        instruction_counter open_cursor_cached_counter(
          "open_cursor_cached_instructions", test::_args.test_name);
        instruction_counter open_cursor_uncached_counter(
          "open_cursor_uncached_instructions", test::_args.test_name);
        instruction_counter timestamp_transaction_uint_counter(
          "timestamp_transaction_uint_instructions", test::_args.test_name);
        std::string cursor_uri = tc->db.get_collection(0).name;

        /* Get the collection to work on. */
        testutil_assert(tc->collection_count == 1);

        scoped_session &session = tc->session;

        int result = begin_transaction_counter.track([&wt_session, &session]() -> int {
            return session->begin_transaction(session.get(), NULL);
        });
        testutil_assert(result == 0);

        make_insert(tc, "1");

        result = commit_transaction_counter.track([&wt_session, &session]() -> int {
            return session->commit_transaction(session.get(), NULL);
        });
        testutil_assert(result == 0);

        result = session->begin_transaction(session.get(), NULL);
        testutil_assert(result == 0);

        result = rollback_trancation_counter.track([&wt_session, &session]() -> int {
            return session->rollback_transaction(session.get(), NULL);
        });
        testutil_assert(result == 0);

        std::string prepare_timestamp = tc->tsm->decimal_to_hex(tc->tsm->get_next_ts());
        result = session->begin_transaction(session.get(), NULL);
        testutil_assert(result == 0);
        make_insert(tc, "3");
        result = prepare_transaction_counter.track([&session, &prepare_timestamp]() -> int {
            return session->prepare_transaction(
              session.get(), ("prepare_timestamp=" + prepare_timestamp).c_str());
        });
        // testutil_assert(result == 0);

        std::string commit_timestamp = tc->tsm->decimal_to_hex(tc->tsm->get_next_ts());

        result =
          commit_after_prepare_transaction_counter.track([&session, &commit_timestamp]() -> int {
              return session->commit_transaction(session.get(),
                ("commit_timestamp=" + commit_timestamp + ",durable_timestamp=" + commit_timestamp)
                  .c_str());
          });
        // testutil_assert(result == 0);

        result = session->begin_transaction(session.get(), NULL);
        testutil_assert(result == 0);
        auto timestamp = tc->tsm->get_next_ts();

        result = timestamp_transaction_uint_counter.track([&session, &timestamp]() -> int {
            return session->timestamp_transaction_uint(
              session.get(), WT_TS_TXN_TYPE_COMMIT, timestamp);
        });
        testutil_assert(result == 0);

        result = session->rollback_transaction(session.get(), NULL);
        testutil_assert(result == 0);

        WT_CURSOR *cursorp = NULL;
        result = open_cursor_cached_counter.track([&session, &cursor_uri, &cursorp]() -> int {
            return session->open_cursor(session.get(), cursor_uri.c_str(), NULL, NULL, &cursorp);
        });
        testutil_assert(result == 0);

        cursorp->close(cursorp);
        cursorp = NULL;

        session->reconfigure(session.get(), "cache_cursors=false");

        result = open_cursor_uncached_counter.track([&session, &cursor_uri, &cursorp]() -> int {
            return session->open_cursor(session.get(), cursor_uri.c_str(), NULL, NULL, &cursorp);
        });
        testutil_assert(result == 0);

        cursorp->close(cursorp);
        cursorp = NULL;
    }
};

} // namespace test_harness

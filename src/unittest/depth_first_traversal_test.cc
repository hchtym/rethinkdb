// Copyright 2010-2013 RethinkDB, all rights reserved.

#include "arch/io/disk.hpp"
#include "btree/btree_store.hpp"
#include "btree/depth_first_traversal.hpp"
#include "concurrency/cond_var.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/proto_utils.hpp"
#include "serializer/config.hpp"
#include "unittest/gtest.hpp"
#include "unittest/unittest_utils.hpp"

namespace unittest {

// RSI: Declare in header file somewhere.
void insert_rows(int start, int finish, btree_store_t<rdb_protocol_t> *store) {
    guarantee(start <= finish);
    for (int i = start; i < finish; ++i) {
        cond_t dummy_interruptor;
        scoped_ptr_t<transaction_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        write_token_pair_t token_pair;
        store->new_write_token_pair(&token_pair);
        store->acquire_superblock_for_write(rwi_write, repli_timestamp_t::invalid,
                                            1, WRITE_DURABILITY_SOFT,
                                            &token_pair, &txn, &superblock, &dummy_interruptor);
        block_id_t sindex_block_id = superblock->get_sindex_block_id();

        std::string data = strprintf("{\"id\" : %d, \"sid\" : %d}", i, i * i);
        rdb_protocol_t::point_write_response_t response;

        store_key_t pk(cJSON_print_primary(scoped_cJSON_t(cJSON_CreateNumber(i)).get(), backtrace_t()));
        rdb_modification_report_t mod_report(pk);
        rdb_set(pk, make_counted<ql::datum_t>(scoped_cJSON_t(cJSON_Parse(data.c_str()))),
                false, store->btree.get(), repli_timestamp_t::invalid, txn.get(),
                superblock.get(), &response, &mod_report.info);

        {
            scoped_ptr_t<buf_lock_t> sindex_block;
            store->acquire_sindex_block_for_write(
                    &token_pair, txn.get(), &sindex_block,
                    sindex_block_id, &dummy_interruptor);

            btree_store_t<rdb_protocol_t>::sindex_access_vector_t sindexes;
            store->aquire_post_constructed_sindex_superblocks_for_write(
                     sindex_block.get(), txn.get(), &sindexes);
            rdb_update_sindexes(sindexes, &mod_report, txn.get());

            mutex_t::acq_t acq;
            store->lock_sindex_queue(sindex_block.get(), &acq);

            write_message_t wm;
            wm << rdb_sindex_change_t(mod_report);

            store->sindex_queue_push(wm, &acq);
        }
    }
}

struct dft_test_callback_t : public depth_first_traversal_callback_t {
    dft_test_callback_t() : count(0) { }

    bool handle_pair(UNUSED const btree_key_t *key, UNUSED const void *value) {
        ++count;
        return true;
    }

    int64_t count;
};

void run_depth_first_traversal_test(direction_t direction) {
    recreate_temporary_directory(base_path_t("."));
    temp_file_t temp_file;

    io_backender_t io_backender(file_direct_io_mode_t::buffered_desired);

    filepath_file_opener_t file_opener(temp_file.name(), &io_backender);
    standard_serializer_t::create(
        &file_opener,
        standard_serializer_t::static_config_t());

    standard_serializer_t serializer(
        standard_serializer_t::dynamic_config_t(),
        &file_opener,
        &get_global_perfmon_collection());

    rdb_protocol_t::store_t store(
            &serializer,
            "unit_test_store",
            GIGABYTE,
            true,
            &get_global_perfmon_collection(),
            NULL,
            &io_backender,
            base_path_t("."));

    insert_rows(0, 100, &store);

    btree_slice_t *slice = store.btree.get();

    scoped_ptr_t<transaction_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    get_btree_superblock_and_txn_for_reading(slice, rwi_read, order_token_t::ignore,
                                             CACHE_SNAPSHOTTED_NO,
                                             &superblock, &txn);

    const int low = 50;
    const int high = 80;

    // A double-closed interval reveals the off-by-one error in the closed test.
    key_range_t range(key_range_t::closed, store_key_t(ql::number_as_pkey(low)),
                      key_range_t::closed, store_key_t(ql::number_as_pkey(high)));

    dft_test_callback_t dft_callback;

    bool res = btree_depth_first_traversal(slice, txn.get(), superblock.get(),
                                           range, &dft_callback, direction);

    ASSERT_TRUE(res);

    ASSERT_EQ(1 + (high - low), dft_callback.count);
}

void run_depth_first_traversal_forward() {
    run_depth_first_traversal_test(direction_t::FORWARD);
}

TEST(DepthFirstTraversal, Forward) {
    run_in_thread_pool(&run_depth_first_traversal_forward);
}

void run_depth_first_traversal_backward() {
    run_depth_first_traversal_test(direction_t::BACKWARD);
}

TEST(DepthFirstTraversal, Backward) {
    run_in_thread_pool(&run_depth_first_traversal_backward);
}


}  // namespace unittest
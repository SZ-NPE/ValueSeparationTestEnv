#!/bin/bash
set -x
export LC_ALL=C
export SCRIPT_HOME=$(pwd)
export ROCKSDB_HOME=$(pwd)/../rocksdb
export HASHKV_HOME=$(pwd)/../HashKV
export DIFFKV_HOME=$(pwd)/../diffkv
export TERARKDB_HOME=$(pwd)/../terarkdb

num=1000000
duration=0
threads=1
max_background_jobs=1
num_multi_db=0
value_size=100
write_buffer_size=64
target_file_size_base=64
max_write_buffer_number=2
monitor_interval=1
db_dir="/mnt/hjx/hjx"
wal_dir="/mnt/sdb/hjx"
db_disk="nvme0n1"
wal_disk="sdb"

direct=true
direct_read=false

disable_wal=false
wal_bytes_per_sync=$((1 * 1048576)) #1m

memtablerep="skip_list"
batch_size=1

const_params="
    --db=${db_dir} \
    --wal_dir=${wal_dir} \
    --num=$((${num} / ${threads})) \
    --num_multi_db=${num_multi_db} \
    --duration=${duration} \
    --threads=${threads} \
    --max_background_jobs=${max_background_jobs} \
    --value_size=${value_size} \
    --write_buffer_size=$((${write_buffer_size} << 20)) \
    --target_file_size_base=$((${target_file_size_base} << 20)) \
    --max_write_buffer_number=${max_write_buffer_number} \
    --histogram=true \
    --use_direct_io_for_flush_and_compaction=${direct} \
    --use_direct_reads=${direct_read} \
    --enable_pipelined_write=false \
    --allow_concurrent_memtable_write=false \
    --disable_wal=${disable_wal} \
    --wal_bytes_per_sync=${wal_bytes_per_sync} \
    --memtablerep=${memtablerep} \
    --batch_size=${batch_size} \
    --compression_ratio=1 \
    --compression_type=none
"

ycsb_params="
    --load_duration=0 \
    --ycsb_workload=${SCRIPT_HOME}/ycsb_workload/workloada \
    --ycsb_request_speed=100 \
    --load_num=100000 \
    --running_num=100000 \
    --random_fill_average=150 
"

# rocksdb
function rocksdb() {
    rm -rf ${db_dir}/*
    rm ${wal_dir}/*
    sudo -S fstrim ${db_dir}
    sudo -S fstrim ${wal_dir}
    sync
    echo 3 >/proc/sys/vm/drop_caches
    ${ROCKSDB_HOME}/build/db_bench \
        ${const_params} \
        --benchmarks=ycsb,stats \
        ${ycsb_params} \
        --use_blob_db=false
}

#hashkv
function hashkv() {
    export LD_LIBRARY_PATH="$HASHKV_HOME/lib/leveldb/out-shared:$HASHKV_HOME/lib/HdrHistogram_c-0.9.4/src:$LD_LIBRARY_PATH"
    cd ${HASHKV_HOME}/bin && mkdir leveldb && mkdir data_dir && rm -f data_dir/* leveldb/*
    cp hashkv_sample_config.ini config.ini
    ./hashkv_test data_dir 100000
    cd ${SCRIPT_HOME}
}
# diffkv
function diffkv() {
    rm -rf ${db_dir}/*
    rm ${wal_dir}/*
    sudo -S fstrim ${db_dir}
    sudo -S fstrim ${wal_dir}
    sync
    echo 3 >/proc/sys/vm/drop_caches
    ${DIFFKV_HOME}/build/titandb_bench \
        ${const_params} \
        --benchmarks=ycsb,stats \
        --${ycsb_params} \
        --use_titan=true \
        --titan_max_background_gc=2 \
        --titan_disable_background_gc=false 
}

# terarkdb
function terarkdb() {
    rm -rf ${db_dir}/*
    rm ${wal_dir}/*
    sudo -S fstrim ${db_dir}
    sudo -S fstrim ${wal_dir}
    sync
    echo 3 >/proc/sys/vm/drop_caches
    ${TERARKDB_HOME}/build/db_bench \
        ${const_params} \
        --benchmarks=ycsb,stats \
        --${ycsb_params} 
}

terarkdb

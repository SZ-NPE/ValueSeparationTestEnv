#prepare testing workspace
mkdir test
cd test
mkdir leveldb
mkdir data_dir
rm -f data_dir/* leveldb/*
du -sh data_dir/ leveldb/

cp ../bin/hashkv_sample_config.ini config.ini
../cmake-build-debug-node11/hashkv_test data_dir 100000 1

cp ../bin/hashkv_config.ini config.ini
../cmake-build-debug-node11/hashkv_test data_dir 41943040 64
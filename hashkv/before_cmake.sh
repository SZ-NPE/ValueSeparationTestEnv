#compile third-party project
export HASHKV_HOME=$(pwd)
cd ${HASHKV_HOME}/lib/HdrHistogram_c-0.9.4
cmake .
make
cd ${HASHKV_HOME}/lib/leveldb
make
cd ${HASHKV_HOME}


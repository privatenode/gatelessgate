@echo off
rem set GPU_FORCE_64BIT_PTR 0
rem set GPU_MAX_HEAP_SIZE 100
rem set GPU_USE_SYNC_OBJECTS 1
rem set GPU_MAX_ALLOC_PERCENT 100
rem set GPU_SINGLE_ALLOC_PERCENT 100
rem gatelessgate.exe --use 0 -c stratum+tcp://us1-zcash.flypool.org:3333 -u t1NwUDeSKu4BxkD58mtEYKDjzw5toiLfmCu.kensaku -p z
gatelessgate.exe --threads 1 --use 0 -c stratum+tcp://equihash.usa.nicehash.com:3357#xnsub -u 1CBgJTch4jSTzSE6mroM7WRJ7Ei7k9LtGx -p x
pause
@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set BIN=out\build-rel\obscli.exe
if not exist "%BIN%" set BIN=out\build\obscli.exe
echo === basic request trace === ...
%BIN% ingest --script examples\basic.jsonl --out examples\basic.sobs --epoch 1 --source aa00000000000000000000000000000001 --boot aaa10000000000000000000000000001
%BIN% trace --archive examples\basic.sobs --request aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1
%BIN% latency --archive examples\basic.sobs --request aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1
echo === retry trace === ...
%BIN% ingest --script examples\retry.jsonl --out examples\retry.sobs --epoch 1 --source aa00000000000000000000000000000001 --boot aaa10000000000000000000000000001
%BIN% explain --archive examples\retry.sobs --request bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1
echo === cold vs warm === ...
%BIN% ingest --script examples\cold_warm.jsonl --out examples\cold.sobs --epoch 1 --source aa00000000000000000000000000000001 --boot aaa10000000000000000000000000001
%BIN% tail --archive examples\cold.sobs
echo === replay+recover === ...
%BIN% replay --archive examples\basic.sobs
%BIN% recover --archive examples\basic.sobs
echo done

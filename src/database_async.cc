/* Copyright (c) 2012-2016 LevelDOWN contributors
 * See list at <https://github.com/level/leveldown#contributing>
 * MIT License <https://github.com/level/leveldown/blob/master/LICENSE.md>
 */

#include <node.h>
#include <node_buffer.h>

#include <leveldb/write_batch.h>
#include <leveldb/filter_policy.h>

#include "database.h"
#include "leveldown.h"
#include "async.h"
#include "database_async.h"

namespace leveldown {

/** OPEN WORKER **/

OpenWorker::OpenWorker (
    Database *database
  , napi_value callback
  , leveldb::Cache* blockCache
  , const leveldb::FilterPolicy* filterPolicy
  , bool createIfMissing
  , bool errorIfExists
  , bool compression
  , uint32_t writeBufferSize
  , uint32_t blockSize
  , uint32_t maxOpenFiles
  , uint32_t blockRestartInterval
) : AsyncWorker(database, callback)
{
  options = new leveldb::Options();
  options->block_cache            = blockCache;
  options->filter_policy          = filterPolicy;
  options->create_if_missing      = createIfMissing;
  options->error_if_exists        = errorIfExists;
  options->compression            = compression
      ? leveldb::kSnappyCompression
      : leveldb::kNoCompression;
  options->write_buffer_size      = writeBufferSize;
  options->block_size             = blockSize;
  options->max_open_files         = maxOpenFiles;
  options->block_restart_interval = blockRestartInterval;
};

OpenWorker::~OpenWorker () {
  delete options;
}

void OpenWorker::Execute () {
  SetStatus(database->OpenDatabase(options));
}

/** CLOSE WORKER **/

CloseWorker::CloseWorker (
    Database *database
  , napi_value callback
) : AsyncWorker(database, callback)
{};

CloseWorker::~CloseWorker () {}

void CloseWorker::Execute () {
  database->CloseDatabase();
}

void CloseWorker::WorkComplete () {
  Napi::HandleScope scope;
  HandleOKCallback();
  delete callback;
  callback = NULL;
}

/** IO WORKER (abstract) **/

IOWorker::IOWorker (
    Database *database
  , napi_value callback
  , leveldb::Slice key
  , v8::Local<v8::Object> &keyHandle
) : AsyncWorker(database, callback)
  , key(key)
{
  Nan::HandleScope scope;

  SaveToPersistent("key", JsValue(keyHandle));
};

IOWorker::~IOWorker () {}

void IOWorker::WorkComplete () {
  Napi::HandleScope scope;

  DisposeStringOrBufferFromSlice(V8LocalValue(GetFromPersistent("key")), key);
  AsyncWorker::WorkComplete();
}

/** READ WORKER **/

ReadWorker::ReadWorker (
    Database *database
  , napi_value callback
  , leveldb::Slice key
  , bool asBuffer
  , bool fillCache
  , v8::Local<v8::Object> &keyHandle
) : IOWorker(database, callback, key, keyHandle)
  , asBuffer(asBuffer)
{
  Napi::HandleScope scope;

  options = new leveldb::ReadOptions();
  options->fill_cache = fillCache;
  SaveToPersistent("key", JsValue(keyHandle));
};

ReadWorker::~ReadWorker () {
  delete options;
}

void ReadWorker::Execute () {
  SetStatus(database->GetFromDatabase(options, key, value));
}

void ReadWorker::HandleOKCallback () {
  Napi::HandleScope scope;

  v8::Local<v8::Value> returnValue;
  if (asBuffer) {
    //TODO: could use NewBuffer if we carefully manage the lifecycle of `value`
    //and avoid an an extra allocation. We'd have to clean up properly when not OK
    //and let the new Buffer manage the data when OK
    returnValue = Nan::CopyBuffer((char*)value.data(), value.size()).ToLocalChecked();
  } else {
    returnValue = Nan::New<v8::String>((char*)value.data(), value.size()).ToLocalChecked();
  }
  napi_value argv[] = {
      napi_get_null(napi_get_current_env())
    , JsValue(returnValue)
  };
  callback->Call(2, argv);
}

/** DELETE WORKER **/

DeleteWorker::DeleteWorker (
    Database *database
  , napi_value callback
  , leveldb::Slice key
  , bool sync
  , v8::Local<v8::Object> &keyHandle
) : IOWorker(database, callback, key, keyHandle)
{
  Napi::HandleScope scope;

  options = new leveldb::WriteOptions();
  options->sync = sync;
  SaveToPersistent("key", JsValue(keyHandle));
};

DeleteWorker::~DeleteWorker () {
  delete options;
}

void DeleteWorker::Execute () {
  SetStatus(database->DeleteFromDatabase(options, key));
}

/** WRITE WORKER **/

WriteWorker::WriteWorker (
    Database *database
  , napi_value callback
  , leveldb::Slice key
  , leveldb::Slice value
  , bool sync
  , v8::Local<v8::Object> &keyHandle
  , v8::Local<v8::Object> &valueHandle
) : DeleteWorker(database, callback, key, sync, keyHandle)
  , value(value)
{
  Napi::HandleScope scope;

  SaveToPersistent("value", JsValue(valueHandle));
};

WriteWorker::~WriteWorker () { }

void WriteWorker::Execute () {
  SetStatus(database->PutToDatabase(options, key, value));
}

void WriteWorker::WorkComplete () {
  Napi::HandleScope scope;

  DisposeStringOrBufferFromSlice(V8LocalValue(GetFromPersistent("value")), value);
  IOWorker::WorkComplete();
}

/** BATCH WORKER **/

BatchWorker::BatchWorker (
    Database *database
  , napi_value callback
  , leveldb::WriteBatch* batch
  , bool sync
) : AsyncWorker(database, callback)
  , batch(batch)
{
  options = new leveldb::WriteOptions();
  options->sync = sync;
};

BatchWorker::~BatchWorker () {
  delete batch;
  delete options;
}

void BatchWorker::Execute () {
  SetStatus(database->WriteBatchToDatabase(options, batch));
}

/** APPROXIMATE SIZE WORKER **/

ApproximateSizeWorker::ApproximateSizeWorker (
    Database *database
  , napi_value callback
  , leveldb::Slice start
  , leveldb::Slice end
  , v8::Local<v8::Object> &startHandle
  , v8::Local<v8::Object> &endHandle
) : AsyncWorker(database, callback)
  , range(start, end)
{
  Napi::HandleScope scope;

  SaveToPersistent("start", JsValue(startHandle));
  SaveToPersistent("end", JsValue(endHandle));
};

ApproximateSizeWorker::~ApproximateSizeWorker () {}

void ApproximateSizeWorker::Execute () {
  size = database->ApproximateSizeFromDatabase(&range);
}

void ApproximateSizeWorker::WorkComplete() {
  Napi::HandleScope scope;

  DisposeStringOrBufferFromSlice(V8LocalValue(GetFromPersistent("start")), range.start);
  DisposeStringOrBufferFromSlice(V8LocalValue(GetFromPersistent("end")), range.limit);
  AsyncWorker::WorkComplete();
}

void ApproximateSizeWorker::HandleOKCallback () {
  Napi::HandleScope scope;
  napi_env env = napi_get_current_env();

  napi_value returnValue = napi_create_number(env, (double) size);
  napi_value argv[] = {
      napi_get_null(env)
    , returnValue
  };
  callback->Call(2, argv);
}

} // namespace leveldown

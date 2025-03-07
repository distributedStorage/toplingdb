#include "include/org_rocksdb_SidePluginRepo.h"
#include "include/org_rocksdb_RocksDB.h"
#include "rocksdb/cache.h"
#include "rocksdb/convenience.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/types.h"
#include "rocksdb/version.h"
#include "rocksjni/cplusplus_to_java_convert.h"
#include "rocksjni/portal.h"

#include <topling/side_plugin_repo.h>

using namespace rocksdb;

template<class OPT>
static void PutOPT
(JNIEnv* env, jobject jrepo, jstring jname, jstring jspec, jobject joptions)
{
  jclass clazz = env->GetObjectClass(joptions);
  jfieldID handleFieldID = env->GetFieldID(clazz, "nativeHandle_", "J"); // long
  OPT* p_opt = (OPT*)env->GetLongField(jrepo, handleFieldID);
  clazz = env->GetObjectClass(jrepo);
  handleFieldID = env->GetFieldID(clazz, "nativeHandle_", "J"); // long
  auto repo = (SidePluginRepo*)env->GetLongField(jrepo, handleFieldID);
  const auto* name = env->GetStringUTFChars(jname, nullptr);
  const auto* spec = env->GetStringUTFChars(jspec, nullptr);
  auto sp_opt = std::make_shared<OPT>(*p_opt);
  repo->Put(name, spec, sp_opt);
  env->ReleaseStringUTFChars(jspec, spec);
  env->ReleaseStringUTFChars(jname, name);
}

extern "C" {
/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    importAutoFile
 * Signature: (Lorg/rocksdb/Slice;)V
 */
void Java_org_rocksdb_SidePluginRepo_importAutoFile
(JNIEnv *env, jobject jrepo, jstring jfname)
{
  const auto* fname = env->GetStringUTFChars(jfname, nullptr);
  ROCKSDB_VERIFY(fname != nullptr);
  jclass clazz = env->GetObjectClass(jrepo);
  jfieldID handleFieldID = env->GetFieldID(clazz, "nativeHandle_", "J"); // long
  auto repo = (SidePluginRepo*)env->GetLongField(jrepo, handleFieldID);
  auto status = repo->ImportAutoFile(fname);
  env->ReleaseStringUTFChars(jfname, fname);
  if (!status.ok()) {
    RocksDBExceptionJni::ThrowNew(env, status);
  }
}

static jobject CreateJDB
(JNIEnv* env, DB* db, ColumnFamilyHandle** cfh_a, size_t cfh_n)
{
  jlongArray jcfh_a = nullptr;
  if (cfh_n) {
    jcfh_a = env->NewLongArray(cfh_n);
    env->SetLongArrayRegion(jcfh_a, 0, jsize(cfh_n), (jlong*)cfh_a);
  }
  jclass clazz = env->FindClass("org/rocksdb/RocksDB");
  jmethodID methodID = env->GetStaticMethodID(clazz, "fromNativeHandles", "(J[J)Lorg/rocksdb/RocksDB;");
  return env->CallStaticObjectMethod(clazz, methodID, db, jcfh_a);
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    nativeOpenDB
 * Signature: (JLjava/lang/String;)Lorg/rocksdb/RocksDB;
 */
jobject Java_org_rocksdb_SidePluginRepo_nativeOpenDB
(JNIEnv* env, jobject jrepo, jlong nativeHandle, jstring jdbname)
{
  DB* db = nullptr;
  auto repo = (SidePluginRepo*)nativeHandle;
  rocksdb::Status status;
  if (jdbname) {
    const auto* dbname = env->GetStringUTFChars(jdbname, nullptr);
    ROCKSDB_VERIFY(dbname != nullptr);
    status = repo->OpenDB(dbname, &db);
    env->ReleaseStringUTFChars(jdbname, dbname);
  } else {
    status = repo->OpenDB(&db);
  }
  if (status.ok()) {
    return CreateJDB(env, db, nullptr, 0);
  } else {
    RocksDBExceptionJni::ThrowNew(env, status);
    return 0;
  }
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    nativeOpenDBMultiCF
 * Signature: (JLjava/lang/String;)Lorg/rocksdb/RocksDB;
 */
jobject Java_org_rocksdb_SidePluginRepo_nativeOpenDBMultiCF
(JNIEnv* env, jobject jrepo, jlong nativeHandle, jstring jdbname)
{
  DB_MultiCF* dbm = nullptr;
  auto repo = (SidePluginRepo*)nativeHandle;
  rocksdb::Status status;
  if (jdbname) {
    const auto* dbname = env->GetStringUTFChars(jdbname, nullptr);
    ROCKSDB_VERIFY(dbname != nullptr);
    status = repo->OpenDB(dbname, &dbm);
    env->ReleaseStringUTFChars(jdbname, dbname);
  } else {
    status = repo->OpenDB(&dbm);
  }
  if (status.ok()) {
    return CreateJDB(env, dbm->db, dbm->cf_handles.data(), dbm->cf_handles.size());
  } else {
    RocksDBExceptionJni::ThrowNew(env, status);
    return nullptr;
  }
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    startHttpServer
 * Signature: ()V
 */
void Java_org_rocksdb_SidePluginRepo_startHttpServer
(JNIEnv* env, jobject jrepo)
{
  jclass clazz = env->GetObjectClass(jrepo);
  jfieldID handleFieldID = env->GetFieldID(clazz, "nativeHandle_", "J"); // long
  auto repo = (SidePluginRepo*)env->GetLongField(jrepo, handleFieldID);
  auto status = repo->StartHttpServer();
  if (!status.ok()) {
    RocksDBExceptionJni::ThrowNew(env, status);
  }
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    closeHttpServer
 * Signature: ()V
 */
void Java_org_rocksdb_SidePluginRepo_closeHttpServer
(JNIEnv* env, jobject jrepo)
{
  jclass clazz = env->GetObjectClass(jrepo);
  jfieldID handleFieldID = env->GetFieldID(clazz, "nativeHandle_", "J"); // long
  auto repo = (SidePluginRepo*)env->GetLongField(jrepo, handleFieldID);
  repo->CloseHttpServer();
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    nativeCloseAllDB
 * Signature: (J)V
 */
void Java_org_rocksdb_SidePluginRepo_nativeCloseAllDB
(JNIEnv* env, jobject jrepo, jlong nativeHandle)
{
  auto repo = (SidePluginRepo*)nativeHandle;
  repo->CloseAllDB(false); // dont close DB and cf
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    put
 * Signature: (Ljava/lang/String;Ljava/lang/String;Lorg/rocksdb/Options;)V
 */
void Java_org_rocksdb_SidePluginRepo_put__Ljava_lang_String_2Ljava_lang_String_2Lorg_rocksdb_Options_2
(JNIEnv* env, jobject jrepo, jstring jname, jstring jspec, jobject joptions)
{
  PutOPT<Options>(env, jrepo, jname, jspec, joptions);
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    put
 * Signature: (Ljava/lang/String;Ljava/lang/String;Lorg/rocksdb/DBOptions;)V
 */
void Java_org_rocksdb_SidePluginRepo_put__Ljava_lang_String_2Ljava_lang_String_2Lorg_rocksdb_DBOptions_2
(JNIEnv* env, jobject jrepo, jstring jname, jstring jspec, jobject joptions)
{
  PutOPT<DBOptions>(env, jrepo, jname, jspec, joptions);
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    put
 * Signature: (Ljava/lang/String;Ljava/lang/String;Lorg/rocksdb/ColumnFamilyOptions;)V
 */
void Java_org_rocksdb_SidePluginRepo_put__Ljava_lang_String_2Ljava_lang_String_2Lorg_rocksdb_ColumnFamilyOptions_2
(JNIEnv* env, jobject jrepo, jstring jname, jstring jspec, jobject joptions)
{
  PutOPT<ColumnFamilyOptions>(env, jrepo, jname, jspec, joptions);
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    newSidePluginRepo
 * Signature: ()J
 */
jlong Java_org_rocksdb_SidePluginRepo_newSidePluginRepo
(JNIEnv* env, jclass clazz)
{
  auto repo = new SidePluginRepo();
  return GET_CPLUSPLUS_POINTER(repo);
}

/*
 * Class:     org_rocksdb_SidePluginRepo
 * Method:    disposeInternal
 * Signature: (J)V
 */
void Java_org_rocksdb_SidePluginRepo_disposeInternal
(JNIEnv* env, jobject jrepo, jlong nativeHandle)
{
  auto repo = (SidePluginRepo*)nativeHandle;
  delete repo;
}

} // extern "C"

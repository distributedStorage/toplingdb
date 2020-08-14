//
// Created by leipeng on 2020-06-29.
//
// json_plugin_repo.h    is mostly for plugin users
// json_plugin_factory.h is mostly for plugin developers
//
#pragma once

#include <memory>
#include <unordered_map>

#include "json_plugin_repo.h"
#include "json.h"
#include "rocksdb/enum_reflection.h"
#include "rocksdb/preproc.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {

using nlohmann::json;

struct JsonPluginRepo::Impl {
  struct ObjInfo {
    std::string name;
    json params; // { class : "class_name", params : "params..." }
  };
  template<class Ptr>
  struct ObjMap {
    std::unordered_map<Ptr, ObjInfo> p2name;
    std::shared_ptr<std::unordered_map<std::string, Ptr>> name2p =
        std::make_shared<std::unordered_map<std::string, Ptr>>();
  };
  template<class T>
  using ObjRepo = ObjMap<std::shared_ptr<T> >;

  ObjRepo<Cache> cache;
  ObjRepo<PersistentCache> persistent_cache;
  ObjRepo<CompactionFilterFactory> compaction_filter_factory;
  ObjMap<const Comparator*> comparator;
  ObjRepo<ConcurrentTaskLimiter> compaction_thread_limiter;
  ObjMap<Env*> env;
  ObjRepo<EventListener> event_listener;
  ObjRepo<FileChecksumGenFactory> file_checksum_gen_factory;
  ObjRepo<FileSystem> file_system;
  ObjRepo<const FilterPolicy> filter_policy;
  ObjRepo<FlushBlockPolicyFactory> flush_block_policy_factory;
  ObjRepo<Logger> info_log;
  ObjRepo<MemoryAllocator> memory_allocator;
  ObjRepo<MemTableRepFactory> mem_table_rep_factory;
  ObjRepo<MergeOperator> merge_operator;
  ObjRepo<RateLimiter> rate_limiter;
  ObjRepo<SstFileManager> sst_file_manager;
  ObjRepo<Statistics> statistics;
  ObjRepo<TableFactory> table_factory;
  ObjRepo<TablePropertiesCollectorFactory> table_properties_collector_factory;
  ObjRepo<TransactionDBMutexFactory> txn_db_mutex_factory;
  ObjRepo<const SliceTransform> slice_transform;

  ObjRepo<Options> options;
  ObjRepo<DBOptions> db_options;
  ObjRepo<ColumnFamilyOptions> cf_options;
  ObjMap<DB_Ptr> db;

  json db_js; // not evaluated during import
};

///@note on principle, the factory itself is stateless, but its product
/// can has states, sometimes we need factory of factory, in this case,
/// just let the factory being PluginFactory:
/// class SomeClass : public PluginFactory<SomeClass*> {...};
/// class SomeClass : public PluginFactory<shared_ptr<SomeClass> > {...};
template<class Ptr>
class PluginFactory {
public:
  virtual ~PluginFactory() {}
  // in some contexts Acquire means 'CreateNew'
  // in some contexts Acquire means 'GetExisting'
  static Ptr AcquirePlugin(const std::string& class_name, const json&,
                                 const JsonPluginRepo&);

  // json is string class_name or
  // object{ class: "class_name", params: {...} }
  static Ptr AcquirePlugin(const json&, const JsonPluginRepo&);

  static Ptr ObtainPlugin(const char* varname, const char* func_name,
                                const json&, const JsonPluginRepo&);

  static Ptr GetPlugin(const char* varname, const char* func_name,
                             const json&, const JsonPluginRepo&);

  static bool HasPlugin(const std::string& class_name);
  static bool SamePlugin(const std::string& clazz1, const std::string& clazz2);

  struct Reg {
    Reg(const Reg&) = delete;
    Reg(Reg&&) = delete;
    Reg& operator=(Reg&&) = delete;
    Reg& operator=(const Reg&) = delete;
    typedef Ptr (*AcqFunc)(const json&,const JsonPluginRepo&);
    using NameToFuncMap = std::unordered_map<std::string, AcqFunc>;
    Reg(Slice class_name, AcqFunc acq);
    ~Reg();
    typename NameToFuncMap::iterator ipos;
    struct Impl;
  };
};
template<class Object>
using PluginFactorySP = PluginFactory<std::shared_ptr<Object> >;
template<class Ptr>
using PluginUpdaterFunc = void(*)(const Ptr&, const json&, const JsonPluginRepo&);
template<class Ptr>
using PluginUpdater = PluginFactory<PluginUpdaterFunc<Ptr> >;

// use SerDeFunc as plugin, register SerDeFunc as plugin
template<class Object>
class SerDeFunc {
 public:
  virtual ~SerDeFunc() {}
  virtual Status Serialize(const Object&, std::string* output) const = 0;
  virtual Status DeSerialize(Object*, const Slice& input) const = 0;
};
template<class Object>
using SerDeFactory = PluginFactory<const SerDeFunc<Object>*>;

template<class Ptr>
struct PluginFactory<Ptr>::Reg::Impl {
  NameToFuncMap func_map;
  std::unordered_map<std::string, Ptr> inst_map;
  static Impl& s_singleton() { static Impl imp; return imp; }
};

template<class Ptr>
PluginFactory<Ptr>::Reg::Reg(Slice class_name, AcqFunc acq) {
  auto& imp = Impl::s_singleton();
  auto ib = imp.func_map.insert(std::make_pair(class_name.ToString(), acq));
  if (!ib.second) {
    fprintf(stderr, "FATAL: %s:%d: %s: duplicate class_name = %s\n"
        , __FILE__, __LINE__, ROCKSDB_FUNC, class_name.data());
    abort();
  }
  if (JsonPluginRepo::DebugLevel() >= 1) {
    fprintf(stderr, "INFO: %s: class = %s\n", ROCKSDB_FUNC, class_name.data());
  }
  this->ipos = ib.first;
}

template<class Ptr>
PluginFactory<Ptr>::Reg::~Reg() {
  auto& imp = Impl::s_singleton();
  imp.func_map.erase(ipos);
}

template<class Ptr>
Ptr
PluginFactory<Ptr>::
AcquirePlugin(const std::string& class_name, const json& js,
              const JsonPluginRepo& repo) {
  auto& imp = Reg::Impl::s_singleton();
  auto iter = imp.func_map.find(class_name);
  if (imp.func_map.end() != iter) {
    Ptr ptr = iter->second(js, repo);
    assert(!!ptr);
    return ptr;
  }
  else {
    //return Ptr(nullptr);
    throw Status::NotFound(ROCKSDB_FUNC, class_name);
  }
}

std::string PluginParseInstID(const std::string& str_val);

template<class Ptr>
Ptr
PluginFactory<Ptr>::
GetPlugin(const char* varname, const char* func_name,
          const json& js, const JsonPluginRepo& repo) {
  if (js.is_string()) {
    const std::string& str_val = js.get<std::string>();
    if (str_val.empty()) {
      throw Status::InvalidArgument(
          func_name, std::string(varname) + " inst_id/class_name is empty");
    }
    Ptr p(nullptr);
    bool ret = false;
    if ('$' == str_val[0]) {
      if (str_val.size() < 3) {
        throw Status::InvalidArgument(func_name,
                   std::string(varname) + " inst_id is too short");
      }
      const auto inst_id = PluginParseInstID(str_val);
      ret = repo.Get(inst_id, &p);
    } else {
      ret = repo.Get(str_val, &p); // the whole str_val is inst_id
    }
    if (!ret) {
      throw Status::NotFound(func_name,
            std::string(varname) + "inst_id = " + str_val);
    }
    assert(!!p);
    return p;
  }
  else {
    throw Status::InvalidArgument(func_name,
      std::string(varname) + " must be a string for reference to object");
  }
}

///@param varname just for error report
///@param func_name just for error report
//
// if json is a string ${inst_id} or $inst_id, then Get the plugin named
// inst_id in repo.
//
// if json is a string does not like ${inst_id} or $inst_id, then the string
// is treated as a class name to create the plugin with empty json params.
//
// if json is an object, it should be { class: class_name, params: ... }
template<class Ptr>
Ptr
PluginFactory<Ptr>::
ObtainPlugin(const char* varname, const char* func_name,
             const json& js, const JsonPluginRepo& repo) {
  if (js.is_string()) {
    const std::string& str_val = js.get<std::string>();
    if (str_val.empty()) {
      throw Status::InvalidArgument(func_name, std::string(varname) +
               " inst_id/class_name is empty");
    }
    if ('$' == str_val[0]) {
      if (str_val.size() < 3) {
        throw Status::InvalidArgument(func_name, std::string(varname) +
                 " inst_id = \"" + str_val + "\" is too short");
      }
      const auto inst_id = PluginParseInstID(str_val);
      Ptr p(nullptr);
      if (!repo.Get(inst_id, &p)) {
        throw Status::NotFound(func_name,
           std::string(varname) + "inst_id = \"" + inst_id + "\"");
      }
      assert(!!p);
      return p;
    } else {
      // string which does not like ${inst_id} or $inst_id
      // try to treat str_val as inst_id to Get it
      Ptr p(nullptr);
      if (repo.Get(str_val, &p)) {
        assert(!!p);
        return p;
      }
      // now treat str_val as class name, try to --
      // AcquirePlugin with empty json params
      const std::string& clazz_name = str_val;
      return AcquirePlugin(clazz_name, json{}, repo);
    }
  } else if (js.is_null()) {
    return Ptr(nullptr);
  } else if (js.is_object()) {
    auto iter = js.find("class");
    if (js.end() == iter) {
        throw std::invalid_argument(std::string(ROCKSDB_FUNC) + ": sub obj class is required");
    }
    if (!iter.value().is_string()) {
        throw std::invalid_argument(std::string(ROCKSDB_FUNC) + ": sub obj class must be string");
    }
    const std::string& clazz_name = iter.value().get<std::string>();
    const json& params = js.at("params");
    return AcquirePlugin(clazz_name, params, repo);
  }
  throw std::invalid_argument(
          std::string(ROCKSDB_FUNC) + ": js must be string, null, or object, but is: " + js.dump());
}

template<class Ptr>
Ptr
PluginFactory<Ptr>::
AcquirePlugin(const json& js, const JsonPluginRepo& repo) {
  if (js.is_string()) {
    const std::string& str_val = js.get<std::string>();
    if (str_val.empty()) {
      throw Status::InvalidArgument(ROCKSDB_FUNC, "jstr class_name is empty");
    }
    // now treat js as class name, try to --
    // AcquirePlugin with empty json params
    const std::string& clazz_name = str_val;
    return AcquirePlugin(clazz_name, json{}, repo);
  } else if (js.is_null()) {
    return Ptr(nullptr);
  } else if (js.is_object()) {
    auto iter = js.find("class");
    if (js.end() == iter) {
      throw Status::InvalidArgument(ROCKSDB_FUNC, "js[\"class\"] is required");
    }
    if (!iter.value().is_string()) {
      throw Status::InvalidArgument(ROCKSDB_FUNC, "js[\"class\"] must be string");
    }
    const std::string& clazz_name = iter.value().get<std::string>();
    const json& params = js.at("params");
    return AcquirePlugin(clazz_name, params, repo);
  }
  throw Status::InvalidArgument(ROCKSDB_FUNC,
        "js must be string, null, or object, but is: " + js.dump());
}

template<class Ptr>
bool PluginFactory<Ptr>::HasPlugin(const std::string& class_name) {
  auto& imp = Reg::Impl::s_singleton();
  return imp.func_map.count(class_name) != 0;
}

// plugin can have alias class name, this function check whether the two
// aliases are defined as a same plugin
template<class Ptr>
bool PluginFactory<Ptr>::SamePlugin(const std::string& clazz1,
                                    const std::string& clazz2) {
  if (clazz1 == clazz2) {
    return true;
  }
  auto& imp = Reg::Impl::s_singleton();
  auto i1 = imp.func_map.find(clazz1);
  auto i2 = imp.func_map.find(clazz2);
  if (imp.func_map.end() == i1) {
    throw Status::NotFound(ROCKSDB_FUNC, "clazz1 = " + clazz1);
  }
  if (imp.func_map.end() == i2) {
    throw Status::NotFound(ROCKSDB_FUNC, "clazz2 = " + clazz2);
  }
  return i1->second == i2->second;
}

const json& jsonRefType();
const JsonPluginRepo& repoRefType();

///@param Name     string of factory class_name
///@param Acquire  must return base class ptr
#define ROCKSDB_FACTORY_REG(Name, Acquire) \
  PluginFactory<decltype(Acquire(jsonRefType(),repoRefType()))>:: \
  Reg ROCKSDB_PP_CAT_3(g_reg_factory_,Acquire,__LINE__)(Name,Acquire)

//////////////////////////////////////////////////////////////////////////////

#define ROCKSDB_JSON_XXX_PROP(js, prop) \
    auto __iter = js.find(#prop); \
    if (js.end() != __iter) try { \
      prop = __iter.value().get<decltype(prop)>(); \
    } catch (const std::exception& ex) {     \
      throw Status::InvalidArgument( \
        ROCKSDB_FUNC, std::string("\"" #prop "\": ") + ex.what()); \
    }

// _REQ_ means 'required'
// _OPT_ means 'optional'
#define ROCKSDB_JSON_REQ_PROP(js, prop) do { \
    ROCKSDB_JSON_XXX_PROP(js, prop)          \
    else throw Status::InvalidArgument(      \
      ROCKSDB_FUNC, "missing required param \"" #prop "\""); \
  } while (0)
#define ROCKSDB_JSON_OPT_PROP(js, prop) do { \
    ROCKSDB_JSON_XXX_PROP(js, prop)          \
  } while (0)
#define ROCKSDB_JSON_REQ_SIZE(js, prop) prop = ParseSizeXiB(js, #prop)
#define ROCKSDB_JSON_OPT_SIZE(js, prop) do try { \
      prop = ParseSizeXiB(js, #prop); \
    } catch (const std::exception&) {} while (0)
#define ROCKSDB_JSON_OPT_ENUM(js, prop) do { \
    auto __iter = js.find(#prop); \
    if (js.end() != __iter) {                \
      if (!__iter.value().is_string())       \
        throw Status::InvalidArgument(       \
          ROCKSDB_FUNC, "enum \"" #prop "\" must be json string"); \
      const std::string& val = __iter.value().get<std::string>(); \
      if (!enum_value(val, &prop)) \
        throw Status::InvalidArgument( \
            ROCKSDB_FUNC, "bad " #prop "=" + val); \
  }} while (0)
#define ROCKSDB_JSON_OPT_NEST(js, prop) \
  do try { \
    auto __iter = js.find(#prop); \
    if (js.end() != __iter) \
      prop = decltype(NestForBase(prop))(__iter.value()); \
  } catch (const std::exception& ex) { \
    throw Status::InvalidArgument(ROCKSDB_FUNC, \
       std::string(#prop ": ") + ex.what()); \
  } while (0)

#define ROCKSDB_JSON_OPT_FACT_INNER(js, prop) \
    prop = PluginFactory<decltype(prop)>:: \
        ObtainPlugin(#prop, ROCKSDB_FUNC, js, repo)
#define ROCKSDB_JSON_OPT_FACT(js, prop) do { \
    auto __iter = js.find(#prop); \
    if (js.end() != __iter) { \
      ROCKSDB_JSON_OPT_FACT_INNER(__iter.value(), prop); \
  }} while (0)

#define ROCKSDB_JSON_SET_PROP(js, prop) js[#prop] = prop
#define ROCKSDB_JSON_SET_ENUM(js, prop) js[#prop] = enum_stdstr(prop)
#define ROCKSDB_JSON_SET_NEST(js, prop) \
  static_cast<const decltype(NestForBase(prop))&>(prop).SaveToJson(js[#prop])

/// for which prop and repo_field with different name
#define ROCKSDB_JSON_SET_FACX(js, prop, repo_field) \
        ROCKSDB_JSON_SET_FACT_INNER(js[#prop], prop, repo_field)

/// this Option and repo has same name prop
#define ROCKSDB_JSON_SET_FACT(js, prop) \
        ROCKSDB_JSON_SET_FACT_INNER(js[#prop], prop, prop)

#define ROCKSDB_JSON_SET_FACT_INNER(inner, prop, repo_field) do { \
  auto& __p2name = repo.m_impl->repo_field.p2name; \
  auto __iter = __p2name.find(prop); \
  if (__p2name.end() != __iter) { \
    if (__iter->second.name.empty()) \
      inner = __iter->second.params; \
    else \
      inner = "${" + __iter->second.name + "}"; \
  } else { \
    fprintf(stderr, \
            "FATAL: %s: can not find name of %s(of %s) by ptr\n", \
            ROCKSDB_FUNC, #prop, #repo_field); \
    abort(); \
  } } while (0)


} // ROCKSDB_NAMESPACE

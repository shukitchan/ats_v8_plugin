#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>

#include "ts/ts.h"
#include "ts/remap.h"
#include "libplatform/libplatform.h"
#include "v8.h"

using std::map;
using std::pair;
using std::string;

using v8::Context;
using v8::EscapableHandleScope;
using v8::External;
using v8::Function;
using v8::FunctionTemplate;
using v8::Global;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Name;
using v8::NamedPropertyHandlerConfiguration;
using v8::NewStringType;
using v8::Object;
using v8::ObjectTemplate;
using v8::PropertyCallbackInfo;
using v8::Script;
using v8::String;
using v8::TryCatch;
using v8::Value;

#define MAX_SCRIPT_FNAME_LENGTH 1024
#define PLUGIN_NAME "v8"
#define EXTERN extern "C"

static std::unique_ptr<v8::Platform> platform = NULL;
static Isolate::CreateParams create_params;
static Isolate* isolate = NULL;

/**
 * The abstract superclass of http request processors.
 */
class HttpRequestProcessor {
 public:
  virtual ~HttpRequestProcessor() { }

  // Initialize this processor.  The map contains options that control
  // how requests should be processed.
  virtual bool Initialize(map<string, string>* options) = 0;

  // Process a single request.
  virtual TSRemapStatus Process() = 0;

  static void Debug(const char* msg);
  static void Error(const char* msg);
};

void HttpRequestProcessor::Debug(const char* msg) {
  TSDebug(PLUGIN_NAME, "%s", msg);
}

void HttpRequestProcessor::Error(const char* msg) {
  TSError("[v8] %s", msg);
}


/**
 * An http request processor that is scriptable using JavaScript.
 */
class JsHttpRequestProcessor : public HttpRequestProcessor {
 public:
  // Creates a new processor that processes requests by invoking the
  // Process function of the JavaScript script given as an argument.
  JsHttpRequestProcessor(Isolate* isolate, Local<String> script)
      : isolate_(isolate), script_(script) {}
  JsHttpRequestProcessor(Isolate* isolate, string file)
      : isolate_(isolate), file_(file) {}
  virtual ~JsHttpRequestProcessor();

  virtual bool Initialize(map<string, string>* opts);
  virtual TSRemapStatus Process();

  Isolate* GetIsolate() { return isolate_; }

 private:
  // Execute the script associated with this processor and extract the
  // Process function.  Returns true if this succeeded, otherwise false.
  bool ExecuteScript(Local<String> script);

  // Wrap the options and output map in a JavaScript objects and
  // install it in the global namespace as 'options' and 'output'.
  bool InstallMaps(map<string, string>* opts);

  // Constructs the template that describes the JavaScript wrapper
  // type for requests.
  //static Local<ObjectTemplate> MakeRequestTemplate(Isolate* isolate);
  static Local<ObjectTemplate> MakeMapTemplate(Isolate* isolate);

  // Callbacks that access maps
  static void MapGet(Local<Name> name, const PropertyCallbackInfo<Value>& info);
  static void MapSet(Local<Name> name, Local<Value> value,
                     const PropertyCallbackInfo<Value>& info);

  // Utility methods for wrapping C++ objects as JavaScript objects,
  // and going back again.
  Local<Object> WrapMap(map<string, string>* obj);
  static map<string, string>* UnwrapMap(Local<Object> obj);

  MaybeLocal<String> ReadFile(Isolate* isolate, const string& name);

  Isolate* isolate_;
  Local<String> script_;
  string file_;
  Global<Context> context_;
  Global<Function> process_;
  static Global<ObjectTemplate> map_template_;
};

static void DebugCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() < 1) return;
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  Local<Value> arg = args[0];
  String::Utf8Value value(isolate, arg);
  HttpRequestProcessor::Debug(*value);
}

static void ErrorCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() < 1) return;
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  Local<Value> arg = args[0];
  String::Utf8Value value(isolate, arg);
  HttpRequestProcessor::Error(*value);
}

JsHttpRequestProcessor::~JsHttpRequestProcessor() {
  // Dispose the persistent handles.  When no one else has any
  // references to the objects stored in the handles they will be
  // automatically reclaimed.
  context_.Reset();
  process_.Reset();
}

Global<ObjectTemplate> JsHttpRequestProcessor::map_template_;

// Execute the script and fetch the Process method.
bool JsHttpRequestProcessor::Initialize(map<string, string>* opts) {
  TSDebug(PLUGIN_NAME, "Initialize()");

  // Create a handle scope to hold the temporary references.
  HandleScope handle_scope(GetIsolate());

  // testing code
  if (!ReadFile(GetIsolate(), file_).ToLocal(&script_)) {
    return false; 
  }

  // Create a template for the global object where we set the
  // built-in global functions.
  Local<ObjectTemplate> global = ObjectTemplate::New(GetIsolate());
  global->Set(String::NewFromUtf8(GetIsolate(), "debug", NewStringType::kNormal)
                  .ToLocalChecked(),
              FunctionTemplate::New(GetIsolate(), DebugCallback));
  global->Set(String::NewFromUtf8(GetIsolate(), "error", NewStringType::kNormal)
                  .ToLocalChecked(),
              FunctionTemplate::New(GetIsolate(), ErrorCallback));

  // Each processor gets its own context so different processors don't
  // affect each other. Context::New returns a persistent handle which
  // is what we need for the reference to remain after we return from
  // this method. That persistent handle has to be disposed in the
  // destructor.
  v8::Local<v8::Context> context = Context::New(GetIsolate(), NULL, global);
  context_.Reset(GetIsolate(), context);

  // Enter the new context so all the following operations take place
  // within it.
  Context::Scope context_scope(context);

  // Make the options mapping available within the context
  if (!InstallMaps(opts))
    return false;

  // Compile and run the script
  if (!ExecuteScript(script_))
    return false;

  // The script compiled and ran correctly.  Now we fetch out the
  // Process function from the global object.
  Local<String> process_name =
      String::NewFromUtf8(GetIsolate(), "Process", NewStringType::kNormal)
          .ToLocalChecked();
  Local<Value> process_val;
  // If there is no Process function, or if it is not a function,
  // bail out
  if (!context->Global()->Get(context, process_name).ToLocal(&process_val) ||
      !process_val->IsFunction()) {
    return false;
  }

  // It is a function; cast it to a Function
  Local<Function> process_fun = Local<Function>::Cast(process_val);

  // Store the function in a Global handle, since we also want
  // that to remain after this call returns
  process_.Reset(GetIsolate(), process_fun);

  // All done; all went well
  return true;
}

bool JsHttpRequestProcessor::InstallMaps(map<string, string>* opts) {
  HandleScope handle_scope(GetIsolate());

  // Wrap the map object in a JavaScript wrapper
  Local<Object> opts_obj = WrapMap(opts);

  v8::Local<v8::Context> context =
      v8::Local<v8::Context>::New(GetIsolate(), context_);

  // Set the options object as a property on the global object.
  context->Global()
      ->Set(context,
            String::NewFromUtf8(GetIsolate(), "options", NewStringType::kNormal)
                .ToLocalChecked(),
            opts_obj)
      .FromJust();

  return true;
}

// Utility function that wraps a C++ http request object in a
// JavaScript object.
Local<Object> JsHttpRequestProcessor::WrapMap(map<string, string>* obj) {
  // Local scope for temporary handles.
  EscapableHandleScope handle_scope(GetIsolate());

  // Fetch the template for creating JavaScript map wrappers.
  // It only has to be created once, which we do on demand.
  if (map_template_.IsEmpty()) {
    Local<ObjectTemplate> raw_template = MakeMapTemplate(GetIsolate());
    map_template_.Reset(GetIsolate(), raw_template);
  }
  Local<ObjectTemplate> templ =
      Local<ObjectTemplate>::New(GetIsolate(), map_template_);

  // Create an empty map wrapper.
  Local<Object> result =
      templ->NewInstance(GetIsolate()->GetCurrentContext()).ToLocalChecked();

  // Wrap the raw C++ pointer in an External so it can be referenced
  // from within JavaScript.
  Local<External> map_ptr = External::New(GetIsolate(), obj);

  // Store the map pointer in the JavaScript wrapper.
  result->SetInternalField(0, map_ptr);

  // Return the result through the current handle scope.  Since each
  // of these handles will go away when the handle scope is deleted
  // we need to call Close to let one, the result, escape into the
  // outer handle scope.
  return handle_scope.Escape(result);
}

// Utility function that extracts the C++ map pointer from a wrapper
// object.
map<string, string>* JsHttpRequestProcessor::UnwrapMap(Local<Object> obj) {
  Local<External> field = Local<External>::Cast(obj->GetInternalField(0));
  void* ptr = field->Value();
  return static_cast<map<string, string>*>(ptr);
}


// Convert a JavaScript string to a std::string.  To not bother too
// much with string encodings we just use ascii.
string ObjectToString(v8::Isolate* isolate, Local<Value> value) {
  String::Utf8Value utf8_value(isolate, value);
  return string(*utf8_value);
}

void JsHttpRequestProcessor::MapGet(Local<Name> name,
                                    const PropertyCallbackInfo<Value>& info) {
  if (name->IsSymbol()) return;

  // Fetch the map wrapped by this object.
  map<string, string>* obj = UnwrapMap(info.Holder());

  // Convert the JavaScript string to a std::string.
  string key = ObjectToString(info.GetIsolate(), Local<String>::Cast(name));

  // Look up the value if it exists using the standard STL ideom.
  map<string, string>::iterator iter = obj->find(key);

  // If the key is not present return an empty handle as signal
  if (iter == obj->end()) return;

  // Otherwise fetch the value and wrap it in a JavaScript string
  const string& value = (*iter).second;
  info.GetReturnValue().Set(
      String::NewFromUtf8(info.GetIsolate(), value.c_str(),
                          NewStringType::kNormal,
                          static_cast<int>(value.length())).ToLocalChecked());
}

void JsHttpRequestProcessor::MapSet(Local<Name> name, Local<Value> value_obj,
                                    const PropertyCallbackInfo<Value>& info) {
  if (name->IsSymbol()) return;

  // Fetch the map wrapped by this object.
  map<string, string>* obj = UnwrapMap(info.Holder());

  // Convert the key and value to std::strings.
  string key = ObjectToString(info.GetIsolate(), Local<String>::Cast(name));
  string value = ObjectToString(info.GetIsolate(), value_obj);

  // Update the map.
  (*obj)[key] = value;

  // Return the value; any non-empty handle will work.
  info.GetReturnValue().Set(value_obj);
}

Local<ObjectTemplate> JsHttpRequestProcessor::MakeMapTemplate(
    Isolate* isolate) {
  EscapableHandleScope handle_scope(isolate);

  Local<ObjectTemplate> result = ObjectTemplate::New(isolate);
  result->SetInternalFieldCount(1);
  result->SetHandler(NamedPropertyHandlerConfiguration(MapGet, MapSet));

  // Again, return the result through the current handle scope.
  return handle_scope.Escape(result);
}

bool JsHttpRequestProcessor::ExecuteScript(Local<String> script) {
  HandleScope handle_scope(GetIsolate());

  // We're just about to compile the script; set up an error handler to
  // catch any exceptions the script might throw.
  TryCatch try_catch(GetIsolate());

  Local<Context> context(GetIsolate()->GetCurrentContext());

  // Compile the script and check for errors.
  Local<Script> compiled_script;
  if (!Script::Compile(context, script).ToLocal(&compiled_script)) {
    String::Utf8Value error(GetIsolate(), try_catch.Exception());
    Error(*error);
    // The script failed to compile; bail out.
    return false;
  }

  // Run the script!
  Local<Value> result;
  if (!compiled_script->Run(context).ToLocal(&result)) {
    // The TryCatch above is still in effect and will have caught the error.
    String::Utf8Value error(GetIsolate(), try_catch.Exception());
    Error(*error);
    // Running the script failed; bail out.
    return false;
  }

  return true;
}

TSRemapStatus JsHttpRequestProcessor::Process() {
 
  // Create a handle scope to keep the temporary object references.
  HandleScope handle_scope(GetIsolate());

  v8::Local<v8::Context> context =
      v8::Local<v8::Context>::New(GetIsolate(), context_);

  // Enter this processor's context so all the remaining operations
  // take place there
  Context::Scope context_scope(context);

  // Set up an exception handler before calling the Process function
  TryCatch try_catch(GetIsolate());

  // Invoke the process function, giving the global object as 'this'
  const int argc = 0;
  v8::Local<v8::Function> process =
      v8::Local<v8::Function>::New(GetIsolate(), process_);
  Local<Value> result;
  if (!process->Call(context, context->Global(), argc, NULL).ToLocal(&result)) {
    String::Utf8Value error(GetIsolate(), try_catch.Exception());
    Error(*error);
    return TSREMAP_NO_REMAP;
  }
  return TSREMAP_NO_REMAP;
}

// Reads a file into a v8 string.
MaybeLocal<String> JsHttpRequestProcessor::ReadFile(Isolate* isolate, const string& name) {
  FILE* file = fopen(name.c_str(), "rb");
  if (file == NULL) return MaybeLocal<String>();

  fseek(file, 0, SEEK_END);
  size_t size = ftell(file);
  rewind(file);

  std::unique_ptr<char> chars(new char[size + 1]);
  chars.get()[size] = '\0';
  for (size_t i = 0; i < size;) {
    i += fread(&chars.get()[i], 1, size - i, file);
    if (ferror(file)) {
      fclose(file);
      return MaybeLocal<String>();
    }
  }
  fclose(file);
  MaybeLocal<String> result = String::NewFromUtf8(
      isolate, chars.get(), NewStringType::kNormal, static_cast<int>(size));
  return result;
}

TSReturnCode
TSRemapInit(TSRemapInterface *, char *, int)
{
  TSDebug(PLUGIN_NAME, "TSRemapInit()");

  // Initialize V8.
  v8::V8::InitializeICUDefaultLocation("/tmp");
  v8::V8::InitializeExternalStartupData("/tmp");
  platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  // create isolate
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  isolate = v8::Isolate::New(create_params);
  //v8::Locker locker(isolate);
  //isolate->Enter();
  //isolate->Exit();
  //v8::Unlocker unlocker(isolate);

  return TS_SUCCESS;
}

TSReturnCode
TSRemapNewInstance(int argc, char *argv[], void **ih, char *errbuf, int errbuf_size)
{
  TSDebug(PLUGIN_NAME, "TSRemapNewInstance()");

  v8::Locker locker(isolate);
  isolate->Enter();

  char script[MAX_SCRIPT_FNAME_LENGTH];  

  if (argc > 2) {
    if (argv[2][0] == '/') {
      snprintf(script, sizeof(script), "%s", argv[2]);
    } else {
      snprintf(script, sizeof(script), "%s/%s", TSConfigDirGet(), argv[2]);
    }
  } else {
    strncpy(errbuf, "[TSRemapNewInstance] - script file is required !!", errbuf_size - 1);
    errbuf[errbuf_size - 1] = '\0';
    return TS_ERROR;
  }

  if (strlen(script) >= MAX_SCRIPT_FNAME_LENGTH - 16) {
    strncpy(errbuf, "[TSRemapNewInstance] - script file name too long !!", errbuf_size - 1);
    errbuf[errbuf_size - 1] = '\0';
    return TS_ERROR;
  }

  TSDebug(PLUGIN_NAME, "TSRemapNewInstance() got file name: %s", script);

  JsHttpRequestProcessor *processor = NULL;

  {
    // creating the processor
    string file(script);
    processor = new JsHttpRequestProcessor(isolate, file);
  }
 
  {
    // no options passed in for now
    map<string, string> options;
    // Initialize the context and process inside the processor , as well as setting up the global object
    if (!processor->Initialize(&options)) {
      strncpy(errbuf, "[TSRemapNewInstance] - Error initializing processor !!", errbuf_size - 1);
      errbuf[errbuf_size - 1] = '\0';
      return TS_ERROR;
    }  
  }

  *ih = processor; 

  isolate->Exit();
  v8::Unlocker unlocker(isolate); 

  return TS_SUCCESS;
}

void
TSRemapDeleteInstance(void *ih)
{
  TSDebug(PLUGIN_NAME, "tsRemapDeleteInstance()");

  v8::Locker locker(isolate);
  isolate->Enter();

  // Getting processor
  JsHttpRequestProcessor *processor = ((JsHttpRequestProcessor *)ih);

  delete processor;

  isolate->Exit();
  v8::Unlocker unlocker(isolate);
}

TSRemapStatus
TSRemapDoRemap(void *ih, TSHttpTxn txn, TSRemapRequestInfo *rri)
{
  TSDebug(PLUGIN_NAME, "TSRemapDoRemap()");

  v8::Locker locker(isolate);
  isolate->Enter();
  
  // Getting processor
  JsHttpRequestProcessor *processor = ((JsHttpRequestProcessor *)ih);

  TSRemapStatus res = processor->Process();

  isolate->Exit();
  v8::Unlocker unlocker(isolate);

  return res;
}

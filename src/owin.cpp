#include <node.h>
#include <v8.h>
#include <uv.h>
#include <vcclr.h>

using namespace v8;
using namespace System::Collections::Generic;
using namespace System::Reflection;
using namespace System::Threading::Tasks;

ref class OwinAppInvokeContext;

typedef struct uv_owin_async_s {
    uv_async_t uv_async;
    gcroot<OwinAppInvokeContext^> context;
} uv_owin_async_t;

ref class OwinAppInvokeContext {
private:
    Dictionary<System::String^,System::Object^>^ netenv;
    Task^ task;
    Persistent<Function>* callback;
    uv_owin_async_t* uv_owin_async;

    void DisposeCallback();
    void DisposeUvOwinAsync();

public:
    OwinAppInvokeContext(Dictionary<System::String^,System::Object^>^ netenv, Handle<Function> callback);
    ~OwinAppInvokeContext();
    !OwinAppInvokeContext();

    void CompleteOnCLRThread(Task^ task);
    void CompleteOnV8Thread();
};

ref class OwinApp {
private:
    System::Object^ instance;
    MethodInfo^ invokeMethod;
    static List<OwinApp^>^ apps;

    OwinApp();

public:
    static OwinApp();
    static Handle<Value> Initialize(const v8::Arguments& args);
    static Handle<Value> Call(const v8::Arguments& args);
};

void completeOnV8Thread(uv_async_t* handle, int status)
{
    System::Console::WriteLine("completeOnV8Thread");
    HandleScope handleScope;
    uv_owin_async_t* uv_owin_async = CONTAINING_RECORD(handle, uv_owin_async_t, uv_async);
    uv_owin_async->context->CompleteOnV8Thread();
}

OwinAppInvokeContext::OwinAppInvokeContext(Dictionary<System::String^,System::Object^>^ netenv, Handle<Function> callback)
{
    this->netenv = netenv;
    this->callback = new Persistent<Function>;
    *(this->callback) = Persistent<Function>::New(callback);
    this->uv_owin_async = new uv_owin_async_t;
    this->uv_owin_async->context = this;
    uv_async_init(uv_default_loop(), &this->uv_owin_async->uv_async, completeOnV8Thread);    
}

OwinAppInvokeContext::~OwinAppInvokeContext()
{
    System::Console::WriteLine("~OwinAppInvokeContext");
    this->!OwinAppInvokeContext();
}

OwinAppInvokeContext::!OwinAppInvokeContext()
{
    System::Console::WriteLine("!OwinAppInvokeContext");
    this->DisposeCallback();
    this->DisposeUvOwinAsync();
}

void OwinAppInvokeContext::DisposeCallback()
{
    if (this->callback)
    {
        System::Console::WriteLine("Disposing callback");
        (*(this->callback)).Dispose();
        (*(this->callback)).Clear();
        delete this->callback;
        this->callback = NULL;        
    }
}

void OwinAppInvokeContext::DisposeUvOwinAsync()
{
    if (this->uv_owin_async)
    {
        System::Console::WriteLine("Disposing uv_owin_async");
        uv_unref((uv_handle_t*)&this->uv_owin_async->uv_async);
        delete this->uv_owin_async;
        this->uv_owin_async = NULL;
    }
}

void OwinAppInvokeContext::CompleteOnCLRThread(Task^ task)
{
    System::Console::WriteLine("CompleteOnCLRThread");
    // TODO: what prevents GC collection of "this" during the thread switch? 
    // Does the gcroot in uv_owin_async->context ensure that?
    this->task = task;
    BOOL ret = PostQueuedCompletionStatus(
        uv_default_loop()->iocp, 
        0, 
        (ULONG_PTR)NULL, 
        &this->uv_owin_async->uv_async.async_req.overlapped);
}

Handle<v8::String> convert(System::String^ text)
{
    HandleScope scope;
    pin_ptr<const wchar_t> message = PtrToStringChars(text);
    return scope.Close(v8::String::New((uint16_t*)message));  
}

void OwinAppInvokeContext::CompleteOnV8Thread()
{
    System::Console::WriteLine("CompleteOnV8Thread");
    HandleScope handleScope;

    this->DisposeUvOwinAsync();

    if (this->callback) 
    {
        Handle<Value> argv[] = { Undefined(), Undefined() };
        int argc = 1;

        switch (this->task->Status) {
            default:
                argv[0] = v8::String::New("The operation reported completion in an unexpected state.");
            break;
            case TaskStatus::Faulted:
                if (this->task->Exception != nullptr) {
                    argv[0] = convert(this->task->Exception->Message);
                }
                else {
                    argv[0] = v8::String::New("The operation has failed with an undetermined error.");
                }
            break;
            case TaskStatus::Canceled:
                argv[0] = v8::String::New("The operation was cancelled.");
            break;
            case TaskStatus::RanToCompletion:
                argc = 2;
                Handle<v8::Object> result = v8::Object::New();
                for each (KeyValuePair<System::String^,System::Object^>^ entry in this->netenv) {
                    if (entry->Value->GetType() == System::String::typeid) {
                        result->Set(
                            convert(entry->Key),
                            convert((System::String^)entry->Value)
                        );
                    }
                }

                argv[1] = result;
            break;
        };

        TryCatch try_catch;
        (*(this->callback))->Call(v8::Context::GetCurrent()->Global(), argc, argv);
        this->DisposeCallback();
        if (try_catch.HasCaught()) {
            node::FatalException(try_catch);
        }        
    }
}

static OwinApp::OwinApp()
{
    OwinApp::apps = gcnew List<OwinApp^>();
}

OwinApp::OwinApp()
{
    // empty
}

Handle<Value> OwinApp::Initialize(const v8::Arguments& args)
{
    HandleScope scope;
    Handle<v8::Object> options = args[0]->ToObject();
    String::Utf8Value assemblyFile(options->Get(String::NewSymbol("assemblyFile")));
    String::Utf8Value typeName(options->Get(String::NewSymbol("typeName")));
    Assembly^ assembly = Assembly::LoadFrom(gcnew System::String(*assemblyFile));
    System::Type^ startupType = assembly->GetType(gcnew System::String(*typeName), false, true);
    OwinApp^ app = gcnew OwinApp();
    app->instance = System::Activator::CreateInstance(startupType, false);
    app->invokeMethod = startupType->GetMethod("Invoke", BindingFlags::Instance | BindingFlags::Public);
    OwinApp::apps->Add(app);

    return Integer::New(OwinApp::apps->Count);
}

void owinAppCompletedOnCLRThread(Task^ task, System::Object^ state)
{
    OwinAppInvokeContext^ context = (OwinAppInvokeContext^)state;
    context->CompleteOnCLRThread(task);
}

Handle<Value> OwinApp::Call(const Arguments& args) 
{
    HandleScope scope;
    int appId = args[0]->Int32Value();
    Handle<v8::Object> jsenv = Handle<v8::Object>::Cast(args[1]);
    Handle<v8::Function> callback = Handle<v8::Function>::Cast(args[2]);
    Dictionary<System::String^,System::Object^>^ netenv = gcnew Dictionary<System::String^,System::Object^>();
    Handle<v8::Array> propertyNames = jsenv->GetPropertyNames();
    for (unsigned int i = 0; i < propertyNames->Length(); i++)
    {
        Handle<v8::String> name = Handle<v8::String>::Cast(propertyNames->Get(i));
        String::Utf8Value utf8name(name);
        String::Utf8Value utf8value(Handle<v8::String>::Cast(jsenv->Get(name)));
        System::String^ netname = gcnew System::String(*utf8name);
        System::String^ netvalue = gcnew System::String(*utf8value);
        netenv->Add(netname, netvalue);
    }

    OwinApp^ app = OwinApp::apps->default[appId - 1];
    Task^ task = (Task^)app->invokeMethod->Invoke(app->instance, gcnew array<System::Object^> { netenv });
    OwinAppInvokeContext^ context = gcnew OwinAppInvokeContext(netenv, callback);
    task->ContinueWith(gcnew System::Action<Task^,System::Object^>(owinAppCompletedOnCLRThread), context);

    return Undefined();
}

Handle<Value> initializeOwinApp(const v8::Arguments& args)
{
    return OwinApp::Initialize(args);
}

Handle<Value> callOwinApp(const v8::Arguments& args)
{
    return OwinApp::Call(args);
}

void init(Handle<Object> target) 
{
    NODE_SET_METHOD(target, "initializeOwinApp", initializeOwinApp);
    NODE_SET_METHOD(target, "callOwinApp", callOwinApp);
}

NODE_MODULE(owin, init);
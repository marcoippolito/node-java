
#include "methodCallBaton.h"
#include "java.h"
#include "javaObject.h"
#include "javaScope.h"

MethodCallBaton::MethodCallBaton(Java* java, jobject method, jarray args, v8::Handle<v8::Value>& callback) {
  JNIEnv *env = java->getJavaEnv();

  m_java = java;
  m_args = (jarray)env->NewGlobalRef(args);
  m_callback = v8::Persistent<v8::Value>::New(callback);
  m_method = env->NewGlobalRef(method);
  m_error = NULL;
  m_result = NULL;
}

MethodCallBaton::~MethodCallBaton() {
  JNIEnv *env = m_java->getJavaEnv();

  if(m_result) {
    env->DeleteGlobalRef(m_result);
  }
  if(m_error) {
    env->DeleteGlobalRef(m_error);
  }
  env->DeleteGlobalRef(m_args);
  env->DeleteGlobalRef(m_method);
  m_callback.Dispose();
}

void MethodCallBaton::run() {
  uv_work_t* req = new uv_work_t();
  req->data = this;
  uv_queue_work(uv_default_loop(), req, MethodCallBaton::EIO_MethodCall, (uv_after_work_cb)MethodCallBaton::EIO_AfterMethodCall);
}

v8::Handle<v8::Value> MethodCallBaton::runSync() {
  JNIEnv *env = m_java->getJavaEnv();
  execute(env);
  return resultsToV8(env);
}

/*static*/ void MethodCallBaton::EIO_MethodCall(uv_work_t* req) {
  MethodCallBaton* self = static_cast<MethodCallBaton*>(req->data);
  JNIEnv *env = javaGetEnv(self->m_java->getJvm(), self->m_java->getClassLoader());
  JavaScope javaScope(env);
  self->execute(env);
}

#if NODE_MINOR_VERSION >= 10
/*static*/ void MethodCallBaton::EIO_AfterMethodCall(uv_work_t* req, int status) {
#else
/*static*/ void MethodCallBaton::EIO_AfterMethodCall(uv_work_t* req) {
#endif
  MethodCallBaton* self = static_cast<MethodCallBaton*>(req->data);
  JNIEnv *env = self->m_java->getJavaEnv();
  JavaScope javaScope(env);
  self->after(env);
  delete req;
  delete self;
}

void MethodCallBaton::after(JNIEnv *env) {
  v8::HandleScope scope;

  if(m_callback->IsFunction()) {
    v8::Local<v8::Function> callback = v8::Function::Cast(*m_callback);
    v8::Handle<v8::Value> result = resultsToV8(env);
    v8::Handle<v8::Value> argv[2];
    if(result->IsNativeError()) {
      argv[0] = result;
      argv[1] = v8::Undefined();
    } else {
      argv[0] = v8::Undefined();
      argv[1] = result;
    }
    node::MakeCallback(v8::Context::GetCurrent()->Global(), callback, 2, argv);
  }
}

v8::Handle<v8::Value> MethodCallBaton::resultsToV8(JNIEnv *env) {
  v8::HandleScope scope;

  if(m_error) {
    jthrowable cause = m_error;

    // if we've caught an InvocationTargetException exception,
    // let's grab the cause. users don't necessarily know that
    // we're invoking the methods through reflection
    jclass invocationExceptionClazz = env->FindClass("java/lang/reflect/InvocationTargetException");
    if (env->IsInstanceOf(m_error, invocationExceptionClazz)) {
      jclass throwableClazz = env->FindClass("java/lang/Throwable");
      jmethodID throwable_getCause = env->GetMethodID(throwableClazz, "getCause", "()Ljava/lang/Throwable;");
      cause = (jthrowable)env->CallObjectMethod(m_error, throwable_getCause);
      assert(!env->ExceptionCheck());
    }

    v8::Handle<v8::Value> err = javaExceptionToV8(m_java, env, cause, m_errorString);
    return scope.Close(err);
  }

  return scope.Close(javaToV8(m_java, env, m_result));
}

void NewInstanceBaton::execute(JNIEnv *env) {
  jclass constructorClazz = env->FindClass("java/lang/reflect/Constructor");
  jmethodID constructor_newInstance = env->GetMethodID(constructorClazz, "newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;");

  //printf("invoke: %s\n", javaMethodCallToString(env, m_method, constructor_newInstance, m_args).c_str());

  jobject result = env->CallObjectMethod(m_method, constructor_newInstance, m_args);
  if(env->ExceptionCheck()) {
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    m_error = (jthrowable)env->NewGlobalRef(ex);
    m_errorString = "Error creating class";
    return;
  }

  m_result = env->NewGlobalRef(result);
}

void StaticMethodCallBaton::execute(JNIEnv *env) {
  jclass methodClazz = env->FindClass("java/lang/reflect/Method");
  jmethodID method_invoke = env->GetMethodID(methodClazz, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");

  /*
  printf("calling %s\n", javaObjectToString(env, m_method).c_str());
  printf("arguments\n");
  for(int i=0; i<env->GetArrayLength(m_args); i++) {
    printf("  %s\n", javaObjectToString(env, env->GetObjectArrayElement((jobjectArray)m_args, i)).c_str());
  }
  */

  jobject result = env->CallObjectMethod(m_method, method_invoke, NULL, m_args);

  if(env->ExceptionCheck()) {
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    m_error = (jthrowable)env->NewGlobalRef(ex);
    m_errorString = "Error running static method";
    return;
  }

  m_result = env->NewGlobalRef(result);
}

void InstanceMethodCallBaton::execute(JNIEnv *env) {
  jclass methodClazz = env->FindClass("java/lang/reflect/Method");
  jmethodID method_invoke = env->GetMethodID(methodClazz, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");

  /*
  printf("calling %s\n", javaObjectToString(env, m_method).c_str());
  printf("arguments\n");
  for(int i=0; i<env->GetArrayLength(m_args); i++) {
    printf("  %s\n", javaObjectToString(env, env->GetObjectArrayElement((jobjectArray)m_args, i)).c_str());
  }
  */

  jobject result = env->CallObjectMethod(m_method, method_invoke, m_javaObject->getObject(), m_args);

  if(env->ExceptionCheck()) {
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    m_error = (jthrowable)env->NewGlobalRef(ex);
    m_errorString = "Error running instance method";
    return;
  }

  if(result == NULL) {
    m_result = NULL;
  } else {
    m_result = env->NewGlobalRef(result);
  }
}

NewInstanceBaton::NewInstanceBaton(
  Java* java,
  jclass clazz,
  jobject method,
  jarray args,
  v8::Handle<v8::Value>& callback) : MethodCallBaton(java, method, args, callback) {
  JNIEnv *env = m_java->getJavaEnv();
  m_clazz = (jclass)env->NewGlobalRef(clazz);
}

NewInstanceBaton::~NewInstanceBaton() {
  JNIEnv *env = m_java->getJavaEnv();
  env->DeleteGlobalRef(m_clazz);
}

StaticMethodCallBaton::StaticMethodCallBaton(
  Java* java,
  jclass clazz,
  jobject method,
  jarray args,
  v8::Handle<v8::Value>& callback) : MethodCallBaton(java, method, args, callback) {
  JNIEnv *env = m_java->getJavaEnv();
  m_clazz = (jclass)env->NewGlobalRef(clazz);
}

StaticMethodCallBaton::~StaticMethodCallBaton() {
  JNIEnv *env = m_java->getJavaEnv();
  env->DeleteGlobalRef(m_clazz);
}

InstanceMethodCallBaton::InstanceMethodCallBaton(
  Java* java,
  JavaObject* obj,
  jobject method,
  jarray args,
  v8::Handle<v8::Value>& callback) : MethodCallBaton(java, method, args, callback) {
  m_javaObject = obj;
  m_javaObject->Ref();
}

InstanceMethodCallBaton::~InstanceMethodCallBaton() {
  m_javaObject->Unref();
}

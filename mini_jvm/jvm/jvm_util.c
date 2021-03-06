//
// Created by gust on 2017/8/8.
//


#include <stdarg.h>
#include "../utils/d_type.h"
#include "jvm.h"
#include <pthread.h>
#include "jvm_util.h"
#include "garbage.h"
#include "java_native_reflect.h"
#include "jdwp.h"

//==================================================================================

void thread_lock_init(ThreadLock *lock) {
    if (lock) {
        pthread_cond_init(&lock->thread_cond, NULL);
        pthread_mutexattr_init(&lock->lock_attr);
        pthread_mutexattr_settype(&lock->lock_attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&lock->mutex_lock, &lock->lock_attr);
    }
}

void thread_lock_dispose(ThreadLock *lock) {
    if (lock) {
        pthread_cond_destroy(&lock->thread_cond);
        pthread_mutexattr_destroy(&lock->lock_attr);
        pthread_mutex_destroy(&lock->mutex_lock);
    }
}


Class *classes_get_c(c8 *clsName) {
    Utf8String *ustr = utf8_create_c(clsName);
    Class *clazz = classes_get(ustr);
    utf8_destory(ustr);
    return clazz;
}

Class *classes_get(Utf8String *clsName) {
    Class *cl = NULL;
    if (clsName) {
        cl = hashtable_get(sys_classloader->classes, clsName);
    }
    return cl;
}

Class *classes_load_get_c(c8 *pclassName, Runtime *runtime) {
    Utf8String *ustr = utf8_create_c(pclassName);
    Class *clazz = classes_load_get(ustr, runtime);
    utf8_destory(ustr);
    return clazz;
}

Class *classes_load_get(Utf8String *ustr, Runtime *runtime) {
    if (!ustr)return NULL;
    Class *cl;
    spin_lock(&sys_classloader->lock);//fast lock
    if (utf8_index_of(ustr, '.') >= 0)
        utf8_replace_c(ustr, ".", "/");
    spin_unlock(&sys_classloader->lock);
    cl = classes_get(ustr);
    if (!cl) {
        garbage_thread_lock();//slow lock
        cl = classes_get(ustr);
        if (!cl) {
            load_class(sys_classloader->g_classpath, ustr, sys_classloader->classes);
        }
        cl = classes_get(ustr);
        garbage_thread_unlock();
        //if (java_debug)event_on_class_prepar(runtime, cl);
    }
    if (cl) {
        class_clinit(cl, runtime);
    }
    return cl;
}

s32 classes_put(Class *clazz) {
    if (clazz) {
        hashtable_put(sys_classloader->classes, clazz->name, clazz);
        garbage_refer_hold(clazz);
        garbage_refer_reg(clazz);
        return 0;
    }
    return -1;
}

Class *array_class_get(Utf8String *desc) {
    if (desc && desc->length && utf8_char_at(desc, 0) == '[') {
        Class *clazz = hashtable_get(array_classloader->classes, desc);
        if (!clazz) {
            garbage_thread_lock();
            clazz = hashtable_get(array_classloader->classes, desc);
            if (!clazz) {
                clazz = class_create();
                clazz->arr_type_index = getDataTypeIndex(utf8_char_at(desc, 1));
                clazz->name = utf8_create_copy(desc);
                hashtable_put(array_classloader->classes, clazz->name, clazz);
                garbage_refer_hold(clazz);
                garbage_refer_reg(clazz);
            }
            garbage_thread_unlock();
        }
        return clazz;
    }
    return NULL;
}

Runtime *threadlist_get(s32 i) {
    Runtime *r = NULL;
    if (i < thread_list->length) {
        r = (Runtime *) arraylist_get_value(thread_list, i);
    }
    return r;
}

void threadlist_remove(Runtime *r) {
    arraylist_remove(thread_list, r);
}

void threadlist_add(Runtime *r) {
    arraylist_push_back(thread_list, r);
}

/**
 * 把utf字符串转为 java unicode 双字节串
 * @param ustr in
 * @param arr out
 */
s32 utf8_2_unicode(Utf8String *ustr, u16 *arr) {
    char *pInput = utf8_cstr(ustr);

    int outputSize = 0; //记录转换后的Unicode字符串的字节数

    char *tmp = (c8 *) arr; //临时变量，用于遍历输出字符串
    while (*pInput) {
        if (*pInput > 0x00 && *pInput <= 0x7F) //处理单字节UTF8字符（英文字母、数字）
        {
            *tmp = *pInput;
            tmp++;
            *tmp = 0; //小端法表示，在高地址填补0
        } else if (((*pInput) & 0xE0) == 0xC0) //处理双字节UTF8字符
        {
            char high = *pInput;
            pInput++;
            char low = *pInput;

            if ((low & 0xC0) != 0x80)  //检查是否为合法的UTF8字符表示
            {
                return -1; //如果不是则报错
            }

            *tmp = (high << 6) + (low & 0x3F);
            tmp++;
            *tmp = (high >> 2) & 0x07;
        } else if (((*pInput) & 0xF0) == 0xE0) //处理三字节UTF8字符
        {
            char high = *pInput;
            pInput++;
            char middle = *pInput;
            pInput++;
            char low = *pInput;

            if (((middle & 0xC0) != 0x80) || ((low & 0xC0) != 0x80)) {
                return -1;
            }

            *tmp = (middle << 6) + (low & 0x7F);
            tmp++;
            *tmp = (high << 4) + ((middle >> 2) & 0x0F);
        } else //对于其他字节数的UTF8字符不进行处理
        {
            return -1;
        }

        pInput++;
        tmp++;
        outputSize += 1;
    }
    return outputSize;
}

int unicode_2_utf8(u16 *jchar_arr, Utf8String *ustr, s32 totalSize) {
    s32 i;
    s32 utf_len = 0;
    for (i = 0; i < totalSize; i++) {
        s32 unic = jchar_arr[i];

        if (unic <= 0x0000007F) {
            // * U-00000000 - U-0000007F:  0xxxxxxx
            utf8_pushback(ustr, unic & 0x7F);

        } else if (unic >= 0x00000080 && unic <= 0x000007FF) {
            // * U-00000080 - U-000007FF:  110xxxxx 10xxxxxx
            utf8_pushback(ustr, ((unic >> 6) & 0x1F) | 0xC0);
            utf8_pushback(ustr, (unic & 0x3F) | 0x80);

        } else if (unic >= 0x00000800 && unic <= 0x0000FFFF) {
            // * U-00000800 - U-0000FFFF:  1110xxxx 10xxxxxx 10xxxxxx
            utf8_pushback(ustr, ((unic >> 12) & 0x0F) | 0xE0);
            utf8_pushback(ustr, ((unic >> 6) & 0x3F) | 0x80);
            utf8_pushback(ustr, (unic & 0x3F) | 0x80);

        } else if (unic >= 0x00010000 && unic <= 0x001FFFFF) {
            // * U-00010000 - U-001FFFFF:  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
            utf8_pushback(ustr, ((unic >> 18) & 0x07) | 0xF0);
            utf8_pushback(ustr, ((unic >> 12) & 0x3F) | 0x80);
            utf8_pushback(ustr, ((unic >> 6) & 0x3F) | 0x80);
            utf8_pushback(ustr, (unic & 0x3F) | 0x80);

        } else if (unic >= 0x00200000 && unic <= 0x03FFFFFF) {
            // * U-00200000 - U-03FFFFFF:  111110xx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
            utf8_pushback(ustr, ((unic >> 24) & 0x03) | 0xF8);
            utf8_pushback(ustr, ((unic >> 18) & 0x3F) | 0x80);
            utf8_pushback(ustr, ((unic >> 12) & 0x3F) | 0x80);
            utf8_pushback(ustr, ((unic >> 6) & 0x3F) | 0x80);
            utf8_pushback(ustr, (unic & 0x3F) | 0x80);

        } else if (unic >= 0x04000000 && unic <= 0x7FFFFFFF) {
            // * U-04000000 - U-7FFFFFFF:  1111110x 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
            utf8_pushback(ustr, ((unic >> 30) & 0x01) | 0xFC);
            utf8_pushback(ustr, ((unic >> 24) & 0x3F) | 0x80);
            utf8_pushback(ustr, ((unic >> 18) & 0x3F) | 0x80);
            utf8_pushback(ustr, ((unic >> 12) & 0x3F) | 0x80);
            utf8_pushback(ustr, ((unic >> 6) & 0x3F) | 0x80);
            utf8_pushback(ustr, (unic & 0x3F) | 0x80);

        }
        utf_len++;
    }
    return i;
}

/**
 * 交换高低位，little endian 和 big endian 互转时用到
 * @param ptr addr
 * @param size len
 */
void swap_endian_little_big(u8 *ptr, s32 size) {
    int i;
    for (i = 0; i < size / 2; i++) {
        u8 tmp = ptr[i];
        ptr[i] = ptr[size - 1 - i];
        ptr[size - 1 - i] = tmp;
    }
}

/*
boolean   4
char  5
float  6
double 7
unsigned char 8
short   9
int  10
long  11
 reference 12
 */
s32 getDataTypeIndex(c8 ch) {
    switch (ch) {
        case 'I':
            return 10;
        case 'L':
        case '[':
            return 12;
        case 'C':
            return 5;
        case 'B':
            return 8;
        case 'Z':
            return 4;
        case 'J':
            return 11;
        case 'F':
            return 6;
        case 'D':
            return 7;
        case 'S':
            return 9;
        default:
            jvm_printf("datatype not found %c\n", ch);
    }
    return 0;
}


u8 getDataTypeTag(s32 index) {
    return data_type_str[index];
}

s32 isDataReferByTag(c8 c) {
    if (c == 'L' || c == '[') {
        return 1;
    }
    return 0;
}

s32 isData8ByteByTag(c8 c) {
    if (c == 'D' || c == 'J') {
        return 1;
    }
    return 0;
}

s32 isDataReferByIndex(s32 index) {
    if (index == DATATYPE_REFERENCE || index == DATATYPE_ARRAY) {
        return 1;
    }
    return 0;
}

/**
 * 从栈中取得实例对象，中间穿插着调用参数
 * @param cmr cmr
 * @param stack stack
 * @return ins
 */
Instance *getInstanceInStack(Class *clazz, ConstantMethodRef *cmr, RuntimeStack *stack) {

    StackEntry entry;
    peek_entry(stack, &entry, stack->size - 1 - cmr->methodParaCount);
    Instance *ins = (Instance *) entry_2_refer(&entry);
    return ins;
}

s32 parseMethodPara(Utf8String *methodType, Utf8String *out) {
    s32 count = 0;
    Utf8String *para = utf8_create_copy(methodType);
    utf8_substring(para, utf8_indexof_c(para, "(") + 1, utf8_last_indexof_c(para, ")"));
    //从后往前拆分方法参数，从栈中弹出放入本地变量
    int i = 0;
    while (para->length > 0) {
        c8 ch = utf8_char_at(para, 0);
        switch (ch) {
            case 'S':
            case 'C':
            case 'B':
            case 'I':
            case 'F':
            case 'Z':
                utf8_substring(para, 1, para->length);
                utf8_append_c(out, "4");
                count++;
                break;
            case 'D':
            case 'J': {
                utf8_substring(para, 1, para->length);
                utf8_append_c(out, "8");
                count += 2;
                break;
            }
            case 'L':
                utf8_substring(para, utf8_indexof_c(para, ";") + 1, para->length);
                utf8_append_c(out, "R");
                count += 1;
                break;
            case '[':
                while (utf8_char_at(para, 1) == '[') {
                    utf8_substring(para, 1, para->length);//去掉多维中的 [[[[LObject; 中的 [符
                }
                if (utf8_char_at(para, 1) == 'L') {
                    utf8_substring(para, utf8_indexof_c(para, ";") + 1, para->length);
                } else {
                    utf8_substring(para, 2, para->length);
                }
                utf8_append_c(out, "R");
                count += 1;
                break;
        }
        i++;
    }
    utf8_destory(para);
    return count;
}

void printDumpOfClasses() {
    HashtableIterator hti;
    hashtable_iterate(sys_classloader->classes, &hti);
    for (; hashtable_iter_has_more(&hti);) {
        Utf8String *k = hashtable_iter_next_key(&hti);
        Class *clazz = classes_get(k);
        jvm_printf("classes entry : hash( %x )%s,%d\n", k->hash, utf8_cstr(k), clazz);
    }
}

s32 sys_properties_load(Utf8String *path) {
    sys_prop = hashtable_create(UNICODE_STR_HASH_FUNC, UNICODE_STR_EQUALS_FUNC);

    FILE *fp = 0;
    Utf8String *filepath = utf8_create_copy(path);
    utf8_append_c(filepath, "sys.properties");
    fp = fopen(utf8_cstr(filepath), "rb");
    utf8_destory(filepath);
    if (fp == 0) {
        jvm_printf("Open file properties failed\n");
        return -1;
    }
    Utf8String *ustr = utf8_create();
    u8 buf[256];
    while (1) {
        u32 len = (u32) fread(buf, 1, 256, fp);
        utf8_append_part_c(ustr, buf, 0, len);
        if (feof(fp)) {
            break;
        }
    }
    fclose(fp);

    //parse
    utf8_replace_c(ustr, "\r\n", "\n");
    utf8_replace_c(ustr, "\r", "\n");
    Utf8String *line = utf8_create();
    while (ustr->length > 0) {
        s32 lineEndAt = utf8_indexof_c(ustr, "\n");
        utf8_clear(line);
        if (lineEndAt >= 0) {
            utf8_append_part(line, ustr, 0, lineEndAt);
            utf8_substring(ustr, lineEndAt + 1, ustr->length);
        } else {
            utf8_append_part(line, ustr, 0, ustr->length);
            utf8_substring(ustr, ustr->length, ustr->length);
        }
        s32 eqAt = utf8_indexof_c(line, "=");
        if (eqAt > 0) {
            Utf8String *key = utf8_create();
            Utf8String *val = utf8_create();
            utf8_append_part(key, line, 0, eqAt);
            utf8_append_part(val, line, eqAt + 1, line->length - (eqAt + 1));
            hashtable_put(sys_prop, key, val);
        }
    }
    utf8_destory(line);
    utf8_destory(ustr);
    return 0;
}

void sys_properties_dispose() {
    HashtableIterator hti;
    hashtable_iterate(sys_prop, &hti);
    for (; hashtable_iter_has_more(&hti);) {
        Utf8String *k = hashtable_iter_next_key(&hti);
        Utf8String *v = hashtable_get(sys_prop, k);
        utf8_destory(k);
        utf8_destory(v);
    }
    hashtable_destory(sys_prop);
}

FILE *logfile = NULL;
s64 last_flush = 0;

void open_log() {
#if _JVM_DEBUG_PRINT_FILE
    if (!logfile) {
        logfile = fopen("./jvmlog.txt", "wb+");
    }
#endif
}

void close_log() {
#if _JVM_DEBUG_PRINT_FILE
    if (!logfile) {
        fclose(logfile);
    }
#endif
}

int jvm_printf(const char *format, ...) {
    //garbage_thread_lock();
    va_list vp;
    va_start(vp, format);
    int result = 0;
#if _JVM_DEBUG_PRINT_FILE
    if (logfile) {

        result = vfprintf(logfile, format, vp);
        if (currentTimeMillis() - last_flush > 1000) {
            fflush(logfile);
            last_flush = currentTimeMillis();
        }
    }
#else
    //result = vprintf(format, vp);
    result = vfprintf(stderr, format, vp);
#endif
    va_end(vp);
    //garbage_thread_unlock();
    return result;
}

void invoke_deepth(Runtime *runtime) {
    garbage_thread_lock();
    int i = 0;
    while (runtime) {
        i++;
        runtime = runtime->parent;
    }
    s32 len = i;

#if _JVM_DEBUG_PRINT_FILE
#ifdef _CYGWIN_CONFIG_H
    fprintf(logfile, "%lx", (s64) (long) pthread_self());
#else
    fprintf(logfile, "%llx", (s64) (long) pthread_self());
#endif //_CYGWIN_CONFIG_H
    for (i = 0; i < len; i++) {
        fprintf(logfile, "  ");
    }
#else
#if __JVM_OS_MAC__ || __JVM_OS_CYGWIN__
    fprintf(stderr, "%llx", (s64) (long) pthread_self());
#else
    fprintf(stderr, "%llx", (s64) (long) pthread_self());
#endif //
    for (i = 0; i < len; i++) {
        fprintf(stderr, "  ");
    }
#endif
    garbage_thread_unlock();
}

//===============================    java 线程  ==================================
s32 jthread_init(Instance *jthread) {
    return jthread_init_with_runtime(jthread, NULL);
}

s32 jthread_init_with_runtime(Instance *jthread, Runtime *runtime) {
    if (!runtime)runtime = runtime_create(NULL);
    localvar_init(runtime, 1);
    jthread_set_stackframe_value(jthread, runtime);
    runtime->clazz = jthread->mb.clazz;
    runtime->threadInfo->jthread = jthread;
    runtime->threadInfo->thread_status = THREAD_STATUS_RUNNING;
    threadlist_add(runtime);

    return 0;
}

s32 jthread_dispose(Instance *jthread) {
    Runtime *runtime = (Runtime *) jthread_get_stackframe_value(jthread);
    threadlist_remove(runtime);
    if (java_debug)event_on_thread_death(runtime->threadInfo->jthread);
    //destory
    jthread_set_stackframe_value(jthread, NULL);
    localvar_dispose(runtime);
    runtime_destory(runtime);

    return 0;
}

void *jtherad_run(void *para) {
    Instance *jthread = (Instance *) para;
    jthread_init(jthread);
    s32 ret = 0;
    Runtime *runtime = (Runtime *) jthread_get_stackframe_value(jthread);
    runtime->threadInfo->pthread = pthread_self();

    Utf8String *methodName = utf8_create_c("run");
    Utf8String *methodType = utf8_create_c("()V");
    MethodInfo *method = NULL;
    method = find_instance_methodInfo_by_name(jthread, methodName, methodType);
    utf8_destory(methodName);
    utf8_destory(methodType);
#if _JVM_DEBUG_BYTECODE_DETAIL > 5
    jvm_printf("therad_loader    %s.%s%s  \n", utf8_cstr(method->_this_class->name),
               utf8_cstr(method->name), utf8_cstr(method->descriptor));
#endif
    garbage_refer_reg(jthread);
    if (java_debug)event_on_thread_start(runtime->threadInfo->jthread);
    runtime->threadInfo->thread_status = THREAD_STATUS_RUNNING;
    push_ref(runtime->stack, (__refer) jthread);
    ret = execute_method(method, runtime, method->_this_class);
    runtime->threadInfo->thread_status = THREAD_STATUS_ZOMBIE;
    jthread_dispose(jthread);
    //jvm_printf("thread over %llx\n", (s64) (long) jthread);
    return para;
}

pthread_t jthread_start(Instance *ins) {//
    pthread_t pt;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&pt, &attr, jtherad_run, ins);
    pthread_attr_destroy(&attr);
    return pt;
}

__refer jthread_get_name_value(Instance *ins) {
    c8 *ptr = getInstanceFieldPtr(ins, ins_field_offset.thread_name);
    return getFieldRefer(ptr);
}

__refer jthread_get_stackframe_value(Instance *ins) {
    c8 *ptr = getInstanceFieldPtr(ins, ins_field_offset.thread_stackFrame);
    return (__refer) (long) getFieldLong(ptr);
}

void jthread_set_stackframe_value(Instance *ins, __refer val) {
    c8 *ptr = getInstanceFieldPtr(ins, ins_field_offset.thread_stackFrame);
    setFieldLong(ptr, (s64) (long) val);
}


void jthreadlock_create(MemoryBlock *mb) {
    garbage_thread_lock();
    if (!mb->thread_lock) {
        ThreadLock *tl = jvm_calloc(sizeof(ThreadLock));
        thread_lock_init(tl);
        mb->thread_lock = tl;
    }
    garbage_thread_unlock();
}

void jthreadlock_destory(MemoryBlock *mb) {
    thread_lock_dispose(mb->thread_lock);
    if (mb->thread_lock) {
        jvm_free(mb->thread_lock);
        mb->thread_lock = NULL;
    }
}

s32 jthread_lock(MemoryBlock *mb, Runtime *runtime) { //可能会重入，同一个线程多次锁同一对象
    if (mb == NULL)return -1;
    if (!mb->thread_lock) {
        jthreadlock_create(mb);
    }
    ThreadLock *jtl = mb->thread_lock;
    //can pause when lock
    while (pthread_mutex_trylock(&jtl->mutex_lock)) {
        check_suspend_and_pause(runtime);
        jthread_yield(runtime);
    }

#if _JVM_DEBUG_BYTECODE_DETAIL > 5
    invoke_deepth(runtime);
    jvm_printf("  lock: %llx   lock holder: %s \n", (s64) (long) (runtime->threadInfo->jthread),
               utf8_cstr(mb->clazz->name));
#endif
    return 0;
}

s32 jthread_unlock(MemoryBlock *mb, Runtime *runtime) {
    if (mb == NULL)return -1;
    if (!mb->thread_lock) {
        jthreadlock_create(mb);
    }
    ThreadLock *jtl = mb->thread_lock;
    pthread_mutex_unlock(&jtl->mutex_lock);
#if _JVM_DEBUG_BYTECODE_DETAIL > 5
    invoke_deepth(runtime);
    jvm_printf("unlock: %llx   lock holder: %s, \n", (s64) (long) (runtime->threadInfo->jthread),
               utf8_cstr(mb->clazz->name));
#endif
    return 0;
}

s32 jthread_notify(MemoryBlock *mb, Runtime *runtime) {
    if (mb == NULL)return -1;
    if (!mb->thread_lock) {
        jthreadlock_create(mb);
    }
    pthread_cond_signal(&mb->thread_lock->thread_cond);
    return 0;
}

s32 jthread_notifyAll(MemoryBlock *mb, Runtime *runtime) {
    if (mb == NULL)return -1;
    if (!mb->thread_lock) {
        jthreadlock_create(mb);
    }
    pthread_cond_broadcast(&mb->thread_lock->thread_cond);
    return 0;
}

s32 jthread_yield(Runtime *runtime) {
    sched_yield();
    return 0;
}

s32 jthread_suspend(Runtime *runtime) {
    runtime->threadInfo->suspend_count++;
    return 0;
}

void jthread_block_enter(Runtime *runtime) {
    runtime->threadInfo->is_blocking = 1;
}

void jthread_block_exit(Runtime *runtime) {
    runtime->threadInfo->is_blocking = 0;
    check_suspend_and_pause(runtime);
}

s32 jthread_resume(Runtime *runtime) {
    if (runtime->threadInfo->suspend_count > 0)runtime->threadInfo->suspend_count--;
    return 0;
}

s32 jthread_waitTime(MemoryBlock *mb, Runtime *runtime, s64 waitms) {
    if (mb == NULL)return -1;
    if (!mb->thread_lock) {
        jthreadlock_create(mb);
    }
    waitms += currentTimeMillis();
    struct timespec t;
    //clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec = waitms / 1000;
    t.tv_nsec = (waitms % 1000) * 1000000;
    runtime->threadInfo->thread_status = THREAD_STATUS_WAIT;
    pthread_cond_timedwait(&mb->thread_lock->thread_cond, &mb->thread_lock->mutex_lock, &t);
    runtime->threadInfo->thread_status = THREAD_STATUS_RUNNING;
    check_suspend_and_pause(runtime);
    return 0;
}

s32 jthread_sleep(Runtime *runtime, s64 ms) {
    runtime->threadInfo->thread_status = THREAD_STATUS_SLEEPING;
    threadSleep(ms);
    runtime->threadInfo->thread_status = THREAD_STATUS_RUNNING;
    check_suspend_and_pause(runtime);
    return 0;
}

s32 check_suspend_and_pause(Runtime *runtime) {
    if (runtime->threadInfo->suspend_count) {
        runtime->threadInfo->is_suspend = 1;
        garbage_thread_lock();
        garbage_thread_notifyall();
        while (runtime->threadInfo->suspend_count) {
            garbage_thread_timedwait(10);
        }
        runtime->threadInfo->is_suspend = 0;
        //jvm_printf(".");
        garbage_thread_unlock();
    }
    return 0;
}

//===============================    实例化数组  ==================================
Instance *jarray_create_by_class(s32 count, Class *clazz) {
    s32 typeIdx = clazz->arr_type_index;
    s32 width = data_type_bytes[typeIdx];
    Instance *arr = jvm_calloc(sizeof(Instance));
    arr->mb.type = MEM_TYPE_ARR;
    arr->mb.clazz = clazz;
    arr->mb.arr_type_index = typeIdx;
    arr->arr_length = count;
    if (arr->arr_length)arr->arr_body = jvm_calloc(width * count);
    return arr;
}

Instance *jarray_create(s32 count, s32 typeIdx, Utf8String *type) {
    Class *clazz = NULL;
    if (type) {
        Utf8String *ustr = utf8_create_c("[");
        if (!isDataReferByTag(utf8_char_at(type, 0)))utf8_append_c(ustr, "L");
        utf8_append(ustr, type);
        if (utf8_char_at(type, type->length - 1) != ';')
            utf8_append_c(ustr, ";");
        clazz = array_class_get(ustr);
        utf8_destory(ustr);
    } else {
        if (!data_type_classes[typeIdx]) {
            Utf8String *ustr = utf8_create_c("[");
            utf8_insert(ustr, ustr->length, getDataTypeTag(typeIdx));
            data_type_classes[typeIdx] = array_class_get(ustr);
            utf8_destory(ustr);
        }
        clazz = data_type_classes[typeIdx];
    }

    Instance *arr = jarray_create_by_class(count, clazz);

    garbage_refer_reg(arr);
    return arr;
}

s32 jarray_destory(Instance *arr) {
    if (arr && arr->mb.type == MEM_TYPE_ARR) {
        jthreadlock_destory(&arr->mb);
        arr->mb.thread_lock = NULL;
        if (arr->arr_body) {
            jvm_free(arr->arr_body);
            arr->arr_body = NULL;
        }
        arr->arr_length = -1;
        jvm_free(arr);
    }
    return 0;
}

/**
 * create multi array
 * @param dim arrdim
 * @param pdesc desc
 * @return ins
 */
Instance *jarray_multi_create(ArrayList *dim, Utf8String *pdesc, s32 deep) {
    s32 len = (s32) (long) arraylist_get_value(dim, dim->length - 1 - deep);
    if (len == -1) {
        return NULL;
    }
    Utf8String *desc = utf8_create_copy(pdesc);
    c8 ch = utf8_char_at(desc, 1);
    Class *clazz = array_class_get(desc);
    Instance *arr = jarray_create_by_class(len, clazz);
    garbage_refer_hold(arr);
    utf8_substring(desc, 1, desc->length);
#if _JVM_DEBUG_BYTECODE_DETAIL > 5
    jvm_printf("multi arr deep :%d  type(%c) arr[%x] size:%d\n", deep, ch, arr, len);
#endif
    if (ch == '[') {
        int i;
        Long2Double l2d;
        for (i = 0; i < len; i++) {
            Instance *elem = jarray_multi_create(dim, desc, deep + 1);
            l2d.r = (__refer) elem;
            jarray_set_field(arr, i, &l2d);
        }
    }
    garbage_refer_release(arr);
    garbage_refer_reg(arr);
    utf8_destory(desc);
    return arr;
}


void jarray_set_field(Instance *arr, s32 index, Long2Double *l2d) {
    s32 idx = arr->mb.arr_type_index;
    s32 bytes = data_type_bytes[idx];
    if (isDataReferByIndex(idx)) {
        setFieldRefer(arr->arr_body + index * bytes, l2d->r);
    } else {
        switch (bytes) {
            case 1:
                setFieldByte(arr->arr_body + index * bytes, (s8) l2d->i2l.i1);
                break;
            case 2:
                setFieldShort(arr->arr_body + index * bytes, (s16) l2d->i2l.i1);
                break;
            case 4:
                setFieldInt(arr->arr_body + index * bytes, (s32) l2d->i2l.i1);
                break;
            case 8:
                setFieldLong(arr->arr_body + index * bytes, l2d->l);
                break;
        }
    }
}

void jarray_get_field(Instance *arr, s32 index, Long2Double *l2d) {
    s32 idx = arr->mb.arr_type_index;
    s32 bytes = data_type_bytes[idx];
    if (isDataReferByIndex(idx)) {
        l2d->r = getFieldRefer(arr->arr_body + index * bytes);
    } else {
        switch (bytes) {
            case 1:
                l2d->i2l.i1 = getFieldByte(arr->arr_body + index * bytes);
                break;
            case 2:
                l2d->i2l.i1 = getFieldShort(arr->arr_body + index * bytes);
                if (idx == DATATYPE_JCHAR) {
                    l2d->i2l.i1 = (u16) l2d->i2l.i1;
                }
                break;
            case 4:
                l2d->i2l.i1 = getFieldInt(arr->arr_body + index * bytes);
                break;
            case 8:
                l2d->l = getFieldLong(arr->arr_body + index * bytes);
                break;
        }
    }
}

//===============================    实例化对象  ==================================
Instance *instance_create(Class *clazz) {
    Instance *ins = jvm_calloc(sizeof(Instance));
    ins->mb.type = MEM_TYPE_INS;
    ins->mb.clazz = clazz;

    ins->obj_fields = jvm_calloc(ins->mb.clazz->field_instance_len);
    garbage_refer_reg(ins);
    return ins;
}

void instance_init(Instance *ins, Runtime *runtime) {
    instance_init_methodtype(ins, runtime, "()V", NULL);
}

void instance_init_methodtype(Instance *ins, Runtime *runtime, c8 *methodtype, RuntimeStack *para) {
    if (ins) {
        Utf8String *methodName = utf8_create_c("<init>");
        Utf8String *methodType = utf8_create_c(methodtype);
        MethodInfo *mi = find_methodInfo_by_name(ins->mb.clazz->name, methodName, methodType);
        push_ref(runtime->stack, (__refer) ins);
        if (para) {
            s32 i;
            for (i = 0; i < para->size; i++) {
                StackEntry entry;
                peek_entry(para, &entry, i);
                push_entry(runtime->stack, &entry);
            }
        }
        execute_method(mi, runtime, ins->mb.clazz);
        utf8_destory(methodName);
        utf8_destory(methodType);
    }
}

void instance_clear_refer(Instance *ins) {
    s32 i;
    Class *clazz = ins->mb.clazz;
    while (clazz) {
        FieldPool *fp = &clazz->fieldPool;
        for (i = 0; i < fp->field_used; i++) {
            FieldInfo *fi = &fp->field[i];
            if ((fi->access_flags & ACC_STATIC) == 0 && isDataReferByIndex(fi->datatype_idx)) {
                c8 *ptr = getInstanceFieldPtr(ins, fi);
                if (ptr) {
                    setFieldRefer(ptr, NULL);
                }
            }
        }
        clazz = getSuperClass(clazz);
    }
}

s32 instance_destory(Instance *ins) {

//    instance_clear_refer(ins);
    jthreadlock_destory(&ins->mb);
    jvm_free(ins->obj_fields);
    jvm_free(ins);


    return 0;
}

/**
 * for java string instance copy
 * deepth copy instance
 * deepth copy array
 *
 * @param src  source instance
 * @return  instance
 */
Instance *instance_copy(Instance *src) {
    Instance *dst = jvm_malloc(sizeof(Instance));
    memcpy(dst, src, sizeof(Instance));
    dst->mb.thread_lock = NULL;
    dst->mb.garbage_reg = 0;
    dst->mb.garbage_mark = 0;
    if (src->mb.type == MEM_TYPE_INS) {
        Class *clazz = src->mb.clazz;
        s32 fileds_len = clazz->field_instance_len;
        if (fileds_len) {
            dst->obj_fields = jvm_malloc(fileds_len);
            memcpy(dst->obj_fields, src->obj_fields, fileds_len);
            s32 i, len;
            while (clazz) {
                FieldPool *fp = &clazz->fieldPool;
                for (i = 0, len = fp->field_used; i < len; i++) {
                    FieldInfo *fi = &fp->field[i];
                    if ((fi->access_flags & ACC_STATIC) == 0 && isDataReferByIndex(fi->datatype_idx)) {
                        c8 *ptr = getInstanceFieldPtr(src, fi);
                        Instance *ins = (Instance *) getFieldRefer(ptr);
                        if (ins) {
                            Instance *new_ins = instance_copy(ins);
                            ptr = getInstanceFieldPtr(dst, fi);
                            setFieldRefer(ptr, new_ins);
                        }
                    }
                }
                clazz = getSuperClass(clazz);
            }
        }
    } else if (src->mb.type == MEM_TYPE_ARR) {
        s32 size = src->arr_length * data_type_bytes[src->mb.arr_type_index];
        dst->arr_body = jvm_malloc(size);
        if (isDataReferByIndex(src->mb.arr_type_index)) {
            s32 i;
            Long2Double l2d;
            for (i = 0; i < dst->arr_length; i++) {
                jarray_get_field(src, i, &l2d);
                if (l2d.r) {
                    l2d.r = instance_copy((Instance *) getFieldRefer(l2d.r));
                    jarray_set_field(dst, i, &l2d);
                }
            }
        } else {
            memcpy(dst->arr_body, src->arr_body, size);
        }
    }
    garbage_refer_reg(dst);
    return dst;
}

//===============================    实例化字符串  ==================================
Instance *jstring_create(Utf8String *src, Runtime *runtime) {
    if (!src)return NULL;
    Utf8String *clsName = utf8_create_c(STR_CLASS_JAVA_LANG_STRING);
    Class *jstr_clazz = classes_load_get(clsName, runtime);
    Instance *jstring = instance_create(jstr_clazz);
    garbage_refer_hold(jstring);//hold for no gc

    jstring->mb.clazz = jstr_clazz;
    instance_init(jstring, runtime);

    c8 *ptr = jstring_get_value_ptr(jstring);
    if (src->length) {
        u16 *buf = jvm_calloc(src->length * data_type_bytes[DATATYPE_JCHAR]);
        s32 len = utf8_2_unicode(src, buf);
        Instance *arr = jarray_create(len, DATATYPE_JCHAR, NULL);//u16 type is 5
        setFieldRefer(ptr, (__refer) arr);//设置数组

        memcpy(arr->arr_body, buf, len * data_type_bytes[DATATYPE_JCHAR]);
        jvm_free(buf);
        jstring_set_count(jstring, len);//设置长度
    } else {
        setFieldRefer(ptr, 0);
    }
    utf8_destory(clsName);
    garbage_refer_release(jstring);
    return jstring;
}

s32 jstring_get_count(Instance *jstr) {
    return getFieldInt(getInstanceFieldPtr(jstr, ins_field_offset.string_count));
}

void jstring_set_count(Instance *jstr, s32 count) {
    setFieldInt(getInstanceFieldPtr(jstr, ins_field_offset.string_count), count);
}

s32 jstring_get_offset(Instance *jstr) {
    return getFieldInt(getInstanceFieldPtr(jstr, ins_field_offset.string_offset));
}

c8 *jstring_get_value_ptr(Instance *jstr) {
    return getInstanceFieldPtr(jstr, ins_field_offset.string_value);
}

Instance *jstring_get_value_array(Instance *jstr) {
    c8 *fieldPtr = jstring_get_value_ptr(jstr);
    Instance *arr = (Instance *) getFieldRefer(fieldPtr);
    return arr;
}

s16 jstring_char_at(Instance *jstr, s32 index) {
    Instance *ptr = jstring_get_value_array(jstr);
    s32 offset = jstring_get_offset(jstr);
    s32 count = jstring_get_count(jstr);
    if (index >= count) {
        return -1;
    }
    if (ptr && ptr->arr_body) {
        u16 *jchar_arr = (u16 *) ptr->arr_body;
        return jchar_arr[offset + index];
    }
    return -1;
}


s32 jstring_index_of(Instance *jstr, uni_char ch, s32 startAt) {
    c8 *fieldPtr = jstring_get_value_ptr(jstr);
    Instance *ptr = (Instance *) getFieldRefer(fieldPtr);//char[]数组实例
    if (ptr && ptr->arr_body) {
        u16 *jchar_arr = (u16 *) ptr->arr_body;
        s32 count = jstring_get_count(jstr);
        s32 offset = jstring_get_offset(jstr);
        s32 i;
        for (i = startAt; i < count; i++) {
            if (jchar_arr[i + offset] == ch) {
                return i;
            }
        }
    }
    return -1;
}

s32 jstring_equals(Instance *jstr1, Instance *jstr2) {
    if (!jstr1 && !jstr2) { //两个都是null
        return 1;
    } else if (!jstr1) {
        return 0;
    } else if (!jstr2) {
        return 0;
    }
    Instance *arr1 = jstring_get_value_array(jstr1);//取得 String[] value
    Instance *arr2 = jstring_get_value_array(jstr2);//取得 String[] value
    s32 count1 = 0, offset1 = 0, count2 = 0, offset2 = 0;
    //0长度字符串可能value[] 是空值，也可能不是空值但count是0
    if (arr1) {
        count1 = jstring_get_count(jstr1);
        offset1 = jstring_get_offset(jstr1);
    }
    if (arr2) {
        count2 = jstring_get_count(jstr2);
        offset2 = jstring_get_offset(jstr2);
    }
    if (count1 != count2) {
        return 0;
    } else if (count1 == 0 && count2 == 0) {
        return 1;
    }
    if (arr1 && arr2 && arr1->arr_body && arr2->arr_body) {
        if (count1 != count2) {
            return 0;
        }
        u16 *jchar_arr1 = (u16 *) arr1->arr_body;
        u16 *jchar_arr2 = (u16 *) arr2->arr_body;
        s32 i;
        for (i = 0; i < count1; i++) {
            if (jchar_arr1[i + offset1] != jchar_arr2[i + offset2]) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

s32 jstring_2_utf8(Instance *jstr, Utf8String *utf8) {
    Instance *arr = jstring_get_value_array(jstr);
    if (arr) {
        s32 count = jstring_get_count(jstr);
        s32 offset = jstring_get_offset(jstr);
        u16 *arrbody = (u16 *) arr->arr_body;
        if (arr->arr_body)unicode_2_utf8(&arrbody[offset], utf8, count);
    }
    return 0;
}
//===============================    例外  ==================================

Instance *exception_create(s32 exception_type, Runtime *runtime) {
#if _JVM_DEBUG_BYTECODE_DETAIL > 5
    jvm_printf("create exception : %s\n", exception_class_name[exception_type]);
#endif
    Utf8String *clsName = utf8_create_c(exception_class_name[exception_type]);
    Class *clazz = classes_load_get(clsName, runtime);
    utf8_destory(clsName);

    Instance *ins = instance_create(clazz);
    garbage_refer_hold(ins);
    instance_init(ins, runtime);
    garbage_refer_release(ins);
    return ins;
}

Instance *exception_create_str(s32 exception_type, Runtime *runtime, c8 *errmsg) {
#if _JVM_DEBUG_BYTECODE_DETAIL > 5
    jvm_printf("create exception : %s\n", exception_class_name[exception_type]);
#endif
    Utf8String *uerrmsg = utf8_create_c(errmsg);
    Instance *jstr = jstring_create(uerrmsg, runtime);
    garbage_refer_hold(jstr);
    utf8_destory(uerrmsg);
    RuntimeStack *para = stack_create(1);
    push_ref(para, jstr);
    garbage_refer_release(jstr);
    Utf8String *clsName = utf8_create_c(exception_class_name[exception_type]);
    Class *clazz = classes_load_get(clsName, runtime);
    utf8_destory(clsName);
    Instance *ins = instance_create(clazz);
    garbage_refer_hold(ins);
    instance_init_methodtype(ins, runtime, "(Ljava/lang/String;)V", para);
    garbage_refer_release(ins);
    stack_destory(para);
    return ins;
}
//===============================    实例操作  ==================================
/**
 * get instance field value address
 * @param ins ins
 * @param fi fi
 * @return addr
 */
c8 *getInstanceFieldPtr(Instance *ins, FieldInfo *fi) {
    return &(ins->obj_fields[fi->_this_class->field_instance_start + fi->offset]);
}

c8 *getStaticFieldPtr(FieldInfo *fi) {
    return &(fi->_this_class->field_static[fi->offset]);
}


inline void setFieldInt(c8 *ptr, s32 v) {
    *((s32 *) ptr) = v;
}

inline void setFieldRefer(c8 *ptr, __refer v) {
    *((__refer *) ptr) = v;
}

inline void setFieldLong(c8 *ptr, s64 v) {
    *((s64 *) ptr) = v;
}

inline void setFieldShort(c8 *ptr, s16 v) {
    *((s16 *) ptr) = v;
}

inline void setFieldByte(c8 *ptr, s8 v) {
    *((s8 *) ptr) = v;
}

inline void setFieldDouble(c8 *ptr, f64 v) {
    *((f64 *) ptr) = v;
}

void setFieldFloat(c8 *ptr, f32 v) {
    *((f32 *) ptr) = v;
}

inline s32 getFieldInt(c8 *ptr) {
    return *((s32 *) ptr);
}

inline __refer getFieldRefer(c8 *ptr) {
    return *((__refer *) ptr);
}

inline s16 getFieldShort(c8 *ptr) {
    return *((s16 *) ptr);
}

inline s8 getFieldByte(c8 *ptr) {
    return *((s8 *) ptr);
}

inline s64 getFieldLong(c8 *ptr) {
    return *((s64 *) ptr);
}

inline f32 getFieldFloat(c8 *ptr) {
    return *((f32 *) ptr);
}


inline f64 getFieldDouble(c8 *ptr) {
    return *((f64 *) ptr);
}


c8 *getFieldPtr_byName_c(Instance *instance, c8 *pclassName, c8 *pfieldName, c8 *pfieldType) {
    Utf8String *clsName = utf8_create_c(pclassName);
    //Class *clazz = classes_get(clsName);

    //set value
    Utf8String *fieldName = utf8_create_c(pfieldName);
    Utf8String *fieldType = utf8_create_c(pfieldType);
    c8 *ptr = getFieldPtr_byName(instance, clsName, fieldName, fieldType);
    utf8_destory(clsName);
    utf8_destory(fieldName);
    utf8_destory(fieldType);
    return ptr;
}


c8 *getFieldPtr_byName(Instance *instance, Utf8String *clsName, Utf8String *fieldName, Utf8String *fieldType) {
    Class *clazz = classes_get(clsName);
    //set value
    s32 fieldIdx = find_constant_fieldref_index(clazz, fieldName, fieldType);
    c8 *ptr = NULL;
    FieldInfo *fi = NULL;
    if (fieldIdx >= 0) {//不在常量表中
        ConstantFieldRef *cfr = find_constant_fieldref(clazz, fieldIdx);
        fi = cfr->fieldInfo;;
    } else {//找字段信息field_info
        fi = find_fieldInfo_by_name(clsName, fieldName, fieldType);
    }
    if (fi) {
        if (fi->access_flags & ACC_STATIC) {
            ptr = getStaticFieldPtr(fi);
        } else {
            ptr = getInstanceFieldPtr(instance, fi);
        }
    }
    return ptr;
}

s32 getLineNumByIndex(CodeAttribute *ca, s32 offset) {
    s32 j;

    for (j = 0; j < ca->line_number_table_length; j++) {
        LineNumberTable *node = &(ca->line_number_table[j]);
        if (offset >= node->start_pc) {
            if (j + 1 < ca->line_number_table_length) {
                LineNumberTable *next_node = &(ca->line_number_table[j + 1]);

                if (offset < next_node->start_pc) {
                    return node->line_number;
                }
            } else {
                return node->line_number;
            }
        }
    }
    return -1;
}


void memoryblock_destory(__refer ref) {
    MemoryBlock *mb = (MemoryBlock *) ref;
    if (!mb)return;
    if (mb->type == MEM_TYPE_INS) {
        instance_destory((Instance *) mb);
    } else if (mb->type == MEM_TYPE_ARR) {
        jarray_destory((Instance *) mb);
    } else if (mb->type == MEM_TYPE_CLASS) {
        class_destory((Class *) mb);
    }
}

JavaThreadInfo *threadinfo_create() {
    JavaThreadInfo *threadInfo = jvm_calloc(sizeof(JavaThreadInfo));
    return threadInfo;
}

void threadinfo_destory(JavaThreadInfo *threadInfo) {
    jvm_free(threadInfo);

}

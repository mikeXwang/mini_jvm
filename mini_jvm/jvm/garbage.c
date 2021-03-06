
#include <errno.h>
#include "jvm.h"
#include "garbage.h"
#include "jvm_util.h"


void dump_refer(void);

void getMBName(void *memblock, Utf8String *name);

void __garbage_clear(void);

void garbage_destory_memobj(MemoryBlock *k);

void _garbage_change_flag(void);

void garbage_copy_refer(void);

s32 garbage_big_search(void);

s32 garbage_pause_the_world(void);

s32 garbage_resume_the_world(void);

s32 checkAndWaitThreadIsSuspend(Runtime *runtime);

void garbage_mark_object(__refer ref);

s32 garbage_copy_refer_thread(Runtime *pruntime);


/**
 * 创建垃圾收集线程，
 *
 * 收集方法如下：
 * 当对象被创建时，注册进垃圾收集器，纳入监管体系。
 *
 *
 * 注册的对象包括 Class 类， Instance 对象实例（包括数组对象）
 *
 * 在垃圾回收时，由垃圾收集线程来收集，收集 collector->objs 中的对象，
 * 收集所有引用计数<=0的对象，直接销毁，释放内存
 *
 * 收集线程会暂停所有正在执行中的java线程 ，回收完之后，恢复线程执行
 *
 * jdwp调试线程中的运行时对象不可回收。
 *
 * collecting  step:
 *
 * 1. stop the world
 * 2. all reg/hold/release operation putin a cache comtainer after stop the world,
 * 3. copy all runtime refer
 * 4. move cache to main objs comtainer
 * 5. resume the world
 * 6. gc
 * 7. restore reg/hold/release opreation
 *
 * @return errorcode
 */


s32 garbage_collector_create() {
    collector = jvm_calloc(sizeof(GcCollector));
    collector->objs_holder = hashset_create();

    collector->runtime_refer_copy = arraylist_create(256);

    collector->_garbage_thread_status = GARBAGE_THREAD_PAUSE;
    thread_lock_init(&collector->garbagelock);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    s32 rc = pthread_create(&collector->_garbage_thread, &attr, collect_thread_run, NULL);
    pthread_attr_destroy(&attr);
    if (rc) {
        jvm_printf("ERROR: garbage thread can't create is %d\n", rc);
    }
    return 0;
}

void garbage_collector_destory() {
    garbage_thread_lock();
    garbage_thread_stop();
    while (collector->_garbage_thread_status != GARBAGE_THREAD_DEAD) {
        garbage_thread_timedwait(50);
    }
    garbage_thread_unlock();
    //
    __garbage_clear();
    //
    hashset_destory(collector->objs_holder);
    collector->objs_holder = NULL;

    arraylist_destory(collector->runtime_refer_copy);

    //
    thread_lock_dispose(&collector->garbagelock);
    jvm_free(collector);
    collector = NULL;
}


void __garbage_clear() {
    jvm_printf("[INFO]garbage clear start========================\n");

    //
    jvm_printf("[INFO]objs size :%lld\n", collector->obj_count);

    //解除所有引用关系后，回收全部对象
    while (garbage_collect());//collect instance

    //release class static field
    classloader_classstatic_clear(sys_classloader);
    classloader_classstatic_clear(array_classloader);
    while (garbage_collect());//collect classes

    //release classes
    classloader_destory(sys_classloader);
    sys_classloader = NULL;
    classloader_destory(array_classloader);
    array_classloader = NULL;
    while (garbage_collect());//collect classes
    //
    jvm_printf("[INFO]objs size :%lld\n", collector->obj_count);

    //dump_refer();
}


//===============================   inner  ====================================

s32 garbage_thread_trylock() {
    return pthread_mutex_trylock(&collector->garbagelock.mutex_lock);
}

void garbage_thread_lock() {
    pthread_mutex_lock(&collector->garbagelock.mutex_lock);
}

void garbage_thread_unlock() {
    pthread_mutex_unlock(&collector->garbagelock.mutex_lock);
}

void garbage_thread_pause() {
    collector->_garbage_thread_status = GARBAGE_THREAD_PAUSE;
}

void garbage_thread_resume() {
    collector->_garbage_thread_status = GARBAGE_THREAD_NORMAL;
}

void garbage_thread_stop() {
    collector->_garbage_thread_status = GARBAGE_THREAD_STOP;
}

void garbage_thread_wait() {
    pthread_cond_wait(&collector->garbagelock.thread_cond, &collector->garbagelock.mutex_lock);
}

void garbage_thread_timedwait(s64 ms) {
    ms += currentTimeMillis();
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000;
    s32 ret = pthread_cond_timedwait(&collector->garbagelock.thread_cond, &collector->garbagelock.mutex_lock, &t);
//    if (ret == ETIMEDOUT) {
//        s32 debug = 1;
//    }
}

void garbage_thread_notify() {
    pthread_cond_signal(&collector->garbagelock.thread_cond);
}

void garbage_thread_notifyall() {
    pthread_cond_broadcast(&collector->garbagelock.thread_cond);
}
//=============================   debug ===================================

void getMBName(void *memblock, Utf8String *name) {

    MemoryBlock *mb = (MemoryBlock *) memblock;
    if (!mb) {
        utf8_append_c(name, "NULL");
    } else {
        switch (mb->type) {
            case MEM_TYPE_CLASS: {
                utf8_append_c(name, "C");
                Class *clazz = collector->_garbage_thread_status == GARBAGE_THREAD_NORMAL ? (Class *) mb : NULL;
                if (clazz)
                    utf8_append(name, clazz->name);
                break;
            }
            case MEM_TYPE_INS: {
                Instance *ins = (Instance *) mb;
                utf8_append_c(name, "L");
                Class *clazz = collector->_garbage_thread_status == GARBAGE_THREAD_NORMAL ? ins->mb.clazz : NULL;
                if (clazz)
                    utf8_append(name, clazz->name);
                utf8_append_c(name, ";");
                break;
            }
            case MEM_TYPE_ARR: {
                Instance *arr = (Instance *) mb;

                utf8_append_c(name, "Array{");
                Class *clazz = collector->_garbage_thread_status == GARBAGE_THREAD_NORMAL ? arr->mb.clazz : NULL;
                if (clazz)
                    utf8_append(name, clazz->name);
                utf8_append_c(name, "}");
                break;
            }
            default:
                utf8_append_c(name, "ERROR");
        }
    }
}


/**
 * 调试用，打印所有引用信息
 */
void dump_refer() {
    //jvm_printf("%d\n",sizeof(struct _Hashset));
    MemoryBlock *mb = collector->tmp_header;
    while (mb) {
        Utf8String *name = utf8_create();
        getMBName(mb, name);
        jvm_printf("   %s[%llx] \n", utf8_cstr(name), (s64) (long) mb);
        utf8_destory(name);
        mb = mb->next;
    }
}


void _garbage_put_in_holder(__refer ref) {
    hashset_put(collector->objs_holder, ref);

#if _JVM_DEBUG_GARBAGE_DUMP
    Utf8String *sus = utf8_create();
    getMBName((MemoryBlock *) ref, sus);
    jvm_printf("+: %s[%llx]\n", utf8_cstr(sus), (s64) (long) ref);
    utf8_destory(sus);
#endif
}

void _garbage_remove_out_holder(__refer ref) {
    hashset_remove(collector->objs_holder, ref, 0);
#if _JVM_DEBUG_GARBAGE_DUMP
    Utf8String *sus = utf8_create();
    getMBName((MemoryBlock *) ref, sus);
    jvm_printf("-: %s[%llx]\n", utf8_cstr(sus), (s64) (long) ref);
    utf8_destory(sus);
#endif
}
//==============================   thread_run() =====================================

void *collect_thread_run(void *para) {
    s64 lastgc = currentTimeMillis();
    while (1) {
        threadSleep(10);

        if (collector->_garbage_thread_status == GARBAGE_THREAD_STOP) {
            break;
        }
        if (collector->_garbage_thread_status == GARBAGE_THREAD_PAUSE) {
            continue;
        }
        if (currentTimeMillis() - lastgc < 1000) {// less than custom sec no gc
            continue;
        }
        if (java_debug && jdwpserver.clients->length) {// less than 3 sec no gc
            continue;
        }
        if (currentTimeMillis() - lastgc > GARBAGE_PERIOD_MS || heap_size > MAX_HEAP_SIZE) {
            garbage_collect();
            lastgc = currentTimeMillis();
        }

    }
    collector->_garbage_thread_status = GARBAGE_THREAD_DEAD;
    return NULL;
}


/**
 * 查找所有实例，如果发现没有被引用时 mb->garbage_mark ，
 * 去除掉此对象对其他对象的引用，并销毁对象
 *
 * @return ret
 */
s64 garbage_collect() {
    collector->isgc = 1;
    s64 mem1 = heap_size;
    s64 del = 0;
    s64 time_startAt;

    time_startAt = currentTimeMillis();
    //prepar gc resource ,
    garbage_thread_lock();

    if (garbage_pause_the_world() != 0) {
        garbage_resume_the_world();
        return -1;
    }
//    jvm_printf("garbage_pause_the_world %lld\n", (currentTimeMillis() - time_startAt));
//    time_startAt = currentTimeMillis();
    if (collector->tmp_header) {
        MemoryBlock *mb = collector->tmp_header;
        while (mb->next) {
            mb = mb->next;
        }
        mb->next = collector->header;//接起来
        collector->header = collector->tmp_header;
        collector->tmp_header = NULL;
    }
//    jvm_printf("garbage_move_cache %lld\n", (currentTimeMillis() - time_startAt));
//    time_startAt = currentTimeMillis();
    garbage_copy_refer();
    //
//    jvm_printf("garbage_copy_refer %lld\n", (currentTimeMillis() - time_startAt));
//    time_startAt = currentTimeMillis();
    s64 obj_count = (collector->obj_count);
    //real GC start
    //
    _garbage_change_flag();
    garbage_big_search();
//    jvm_printf("garbage_big_search %lld\n", (currentTimeMillis() - time_startAt));
//    time_startAt = currentTimeMillis();
    garbage_resume_the_world();
    garbage_thread_unlock();

//    jvm_printf("garbage_resume_the_world %lld\n", (currentTimeMillis() - time_startAt));
    s64 time_stopWorld = currentTimeMillis() - time_startAt;
    time_startAt = currentTimeMillis();
    //

    MemoryBlock *nextmb = collector->header;
    MemoryBlock *curmb, *prevmb = NULL;
    s64 iter = 0;
    while (nextmb) {
        iter++;
        curmb = nextmb;
        nextmb = curmb->next;
        if (curmb->garbage_mark != collector->flag_refer) {
            garbage_destory_memobj(curmb);
            if (prevmb)prevmb->next = nextmb;
            else collector->header = nextmb;
            del++;

        } else {
            prevmb = curmb;
        }
    }
    spin_lock(&collector->lock);
    collector->obj_count -= del;
    spin_unlock(&collector->lock);

    s64 time_gc = currentTimeMillis() - time_startAt;
    jvm_printf("[INFO]gc obj: %lld->%lld   heap : %lld -> %lld  stop_world: %lld  gc:%lld\n",
               obj_count, collector->obj_count, mem1, heap_size, time_stopWorld, time_gc);


    collector->isgc = 0;
    return del;
}

void _garbage_change_flag() {
    collector->flag_refer++;
    if (collector->flag_refer == 0) {
        collector->flag_refer = 1;
    }
}


void garbage_destory_memobj(MemoryBlock *mb) {
#if _JVM_DEBUG_GARBAGE_DUMP
    Utf8String *sus = utf8_create();
    getMBName(mb, sus);
    jvm_printf("X: %s[%llx]\n", utf8_cstr(sus), (s64) (long) mb);
    utf8_destory(sus);
#endif
    memoryblock_destory((Instance *) mb);
}


/**
 * 各个线程把自己还需要使用的对象进行标注，表示不能回收
 */
void _list_iter_thread_pause(ArrayListValue value, void *para) {
    jthread_suspend((Runtime *) value);
}

s32 garbage_pause_the_world(void) {
    s32 i;
    //jvm_printf("thread size:%d\n", thread_list->length);

    if (thread_list->length) {
        arraylist_iter_safe(thread_list, _list_iter_thread_pause, NULL);

        for (i = 0; i < thread_list->length; i++) {
            Runtime *runtime = arraylist_get_value(thread_list, i);
            if (checkAndWaitThreadIsSuspend(runtime) == -1) {
                return -1;
            }
#if _JVM_DEBUG_GARBAGE_DUMP
            Utf8String *stack = utf8_create();
            getRuntimeStack(runtime, stack);
            jvm_printf("%s\n", utf8_cstr(stack));
            utf8_destory(stack);
#endif
        }

    }
    //调试线程
    if (java_debug) {
        Runtime *runtime = jdwpserver.runtime;
        if (runtime) {
            jthread_suspend(runtime);
            if (checkAndWaitThreadIsSuspend(runtime) == -1) {
                return -1;
            }
        }
    }
    return 0;
}

s32 garbage_resume_the_world() {
    s32 i;
    for (i = 0; i < thread_list->length; i++) {
        Runtime *runtime = arraylist_get_value(thread_list, i);
        if (runtime) {
#if _JVM_DEBUG_GARBAGE_DUMP
            Utf8String *stack = utf8_create();
            getRuntimeStack(runtime, stack);
            jvm_printf("%s\n", utf8_cstr(stack));
            utf8_destory(stack);
#endif
            jthread_resume(runtime);
            garbage_thread_notifyall();
        }
    }

    //调试线程
    if (java_debug) {
        Runtime *runtime = jdwpserver.runtime;
        if (runtime) {
            jthread_resume(runtime);
        }
    }
    return 0;
}


s32 checkAndWaitThreadIsSuspend(Runtime *runtime) {
    while (!(runtime->threadInfo->is_suspend) &&
           !(runtime->threadInfo->thread_status == THREAD_STATUS_SLEEPING) &&
           !(runtime->threadInfo->thread_status == THREAD_STATUS_WAIT) &&
           !(runtime->threadInfo->is_blocking)) { // if a native method blocking , must set thread status is wait before enter native method
        garbage_thread_timedwait(100);
        if (collector->_garbage_thread_status != GARBAGE_THREAD_NORMAL) {
            return -1;
        }
    }
    return 0;
}

//=================================  big_search ==================================

/**
 * on all threads stoped ,
 * mark thread's localvar and stack refered obj and deep search
 * @return ret
 */
s32 garbage_big_search() {
    s32 i, len;
    for (i = 0, len = collector->runtime_refer_copy->length; i < len; i++) {
        __refer r = arraylist_get_value(collector->runtime_refer_copy, i);
        garbage_mark_object(r);
    }

    HashsetIterator hi;
    hashset_iterate(collector->objs_holder, &hi);
    while (hashset_iter_has_more(&hi)) {
        HashsetKey k = hashset_iter_next_key(&hi);

        garbage_mark_object(k);
    }

    return 0;
}

void garbage_copy_refer() {
    arraylist_clear(collector->runtime_refer_copy);
    s32 i;
    //jvm_printf("thread set size:%d\n", thread_list->length);
    for (i = 0; i < thread_list->length; i++) {
        Runtime *runtime = threadlist_get(i);
        garbage_copy_refer_thread(runtime);
    }
//    arraylist_iter_safe(thread_list, _list_iter_iter_copy, NULL);
    //调试线程
    if (java_debug) {
        Runtime *runtime = jdwpserver.runtime;
        if (runtime) {
            garbage_copy_refer_thread(runtime);
        }
    }
}

/**
 * 判定某个对象是否被所有线程的runtime引用
 * 被运行时的栈或局部变量所引用，
 * 这两种情况下，对象是不能被释放的
 *
 * @param pruntime son of runtime
 * @return how many marked
 */


s32 garbage_copy_refer_thread(Runtime *pruntime) {
    arraylist_push_back(collector->runtime_refer_copy, pruntime->threadInfo->jthread);

    s32 i;
    StackEntry entry;
    Runtime *runtime = pruntime;
    RuntimeStack *stack = runtime->stack;
    for (i = 0; i < stack->size; i++) {
        peek_entry(stack, &entry, i);
        if (is_ref(&entry)) {
            __refer ref = entry_2_refer(&entry);
            if (ref) {
                //garbage_mark_object(ref);
                arraylist_push_back(collector->runtime_refer_copy, ref);
            }
        }
    }
    while (runtime) {
        for (i = 0; i < runtime->localvar_count; i++) {
            __refer ref = runtime->localvar[i].refer;
            if (ref) {
                //garbage_mark_object(ref);
                arraylist_push_back(collector->runtime_refer_copy, ref);
            }
        }
        runtime = runtime->son;
    }
    //jvm_printf("[%llx] notified\n", (s64) (long) pruntime->threadInfo->jthread);
    return 0;
}


static inline void instance_mark_refer(Instance *ins) {
    s32 i, len;
    Class *clazz = ins->mb.clazz;
    while (clazz) {
        FieldPool *fp = &clazz->fieldPool;
        for (i = 0, len = fp->field_used; i < len; i++) {
            FieldInfo *fi = &fp->field[i];
            if ((fi->access_flags & ACC_STATIC) == 0 && isDataReferByIndex(fi->datatype_idx)) {
                c8 *ptr = getInstanceFieldPtr(ins, fi);
                if (ptr) {
                    __refer ref = getFieldRefer(ptr);
                    garbage_mark_object(ref);
                }
            }
        }
        clazz = getSuperClass(clazz);
    }
}


static inline void jarray_mark_refer(Instance *arr) {
    if (arr && arr->mb.type == MEM_TYPE_ARR) {
        if (isDataReferByIndex(arr->mb.arr_type_index)) {
            s32 i;
            Long2Double l2d;
            for (i = 0; i < arr->arr_length; i++) {//把所有引用去除，否则不会垃圾回收
                l2d.l = 0;
                jarray_get_field(arr, i, &l2d);
                garbage_mark_object(l2d.r);
            }
        }
    }
    return;
}

/**
 * mark class static field is used
 * @param clazz class
 */
static inline void class_mark_refer(Class *clazz) {
    s32 i, len;
    if (clazz->field_static) {
        FieldPool *fp = &clazz->fieldPool;
        for (i = 0, len = fp->field_used; i < len; i++) {
            FieldInfo *fi = &fp->field[i];
            if ((fi->access_flags & ACC_STATIC) != 0 && isDataReferByIndex(fi->datatype_idx)) {
                c8 *ptr = getStaticFieldPtr(fi);
                if (ptr) {
                    __refer ref = getFieldRefer(ptr);
                    garbage_mark_object(ref);
                }
            }
        }
    }
}


/**
 * 递归标注obj所有的子孙
 * @param ref addr
 */

void garbage_mark_object(__refer ref) {
    if (ref) {
        MemoryBlock *mb = (MemoryBlock *) ref;
        if (collector->flag_refer != mb->garbage_mark) {
            mb->garbage_mark = collector->flag_refer;
            switch (mb->type) {
                case MEM_TYPE_INS:
                    instance_mark_refer((Instance *) mb);
                    break;
                case MEM_TYPE_ARR:
                    jarray_mark_refer((Instance *) mb);
                    break;
                case MEM_TYPE_CLASS:
                    class_mark_refer((Class *) mb);
                    break;
            }
        }
    }
}
//=================================  reg unreg ==================================

MemoryBlock *garbage_is_alive(__refer ref) {
    __refer result = hashset_get(collector->objs_holder, ref);
    spin_lock(&collector->lock);
    if (!result) {
        MemoryBlock *mb = collector->header;
        while (mb) {
            if (mb == ref) {
                result = mb;
            }
            mb = mb->next;
        }
    }
    if (!result) {
        MemoryBlock *mb = collector->tmp_header;
        while (mb) {
            if (mb == ref) {
                result = mb;
            }
            mb = mb->next;
        }
    }
    spin_unlock(&collector->lock);
    return (MemoryBlock *) result;
}


s32 garbage_refer_reg(__refer ref) {
    if (ref) {
        MemoryBlock *mb = (MemoryBlock *) ref;
        spin_lock(&collector->lock);
        if (!mb->garbage_reg) {
            mb->garbage_reg = 1;
            collector->obj_count++;
            if (collector->isgc) {
                mb->next = collector->tmp_header;
                collector->tmp_header = mb;
            } else {
                mb->next = collector->header;
                collector->header = mb;
            }
#if _JVM_DEBUG_GARBAGE_DUMP
            Utf8String *sus = utf8_create();
            getMBName((MemoryBlock *) ref, sus);
            jvm_printf("R: %s[%llx]\n", utf8_cstr(sus), (s64) (long) ref);
            utf8_destory(sus);
#endif
        }
        spin_unlock(&collector->lock);
    }
    return 0;
}


void garbage_refer_hold(__refer ref) {
    if (ref) {
        _garbage_put_in_holder(ref);
    }
}

void garbage_refer_release(__refer ref) {
    if (ref) {
        _garbage_remove_out_holder(ref);
    }
}

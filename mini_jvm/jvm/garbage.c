
#include "jvm.h"
#include "garbage.h"
#include "jvm_util.h"


void getMemBlockName(void *memblock, Utf8String *name);

void garbage_destory_memobj(Runtime *runtime, __refer k);

s32 garbage_mark_refered_obj(Runtime *pruntime);

void garbage_derefer_all(void *parentPtr, Runtime *runtime);

s32 garbage_mark_by_threads(Runtime *runtime);

/**
 * 创建垃圾收集线程，
 *
 * 收集方法如下：
 * 所有新创建的对象，向垃圾收集器注册，表示这个对象将来可能会被回收，纳入监管体系。
 * 同时向 son_2_father，father_2_son 注册，
 * son_2_father 记录了子对象对父对象的引用，同时在father_2_son中记录父对象对子对象的引用。
 *
 * 建立和解除引用关系，比如：
 * Class Foo{
 *     Object var;
 *
 *     void m(Object obj1, Object obj2){
 *         var= obj1;  //建立 Foo实例 与 obj1 的引用关系
 *         var= obj2;  //解除 Foo实例 与 obj1 的引用关系， 建立 Foo实例 与 obj2 的引用关系
 *     }
 * }
 *
 * 注册的对象包括 Class 类， Instance 对象实例（包括数组对象）
 *
 * 在垃圾回收时，由单独的线程来异步收集，主要收集 son_2_father 中的对象，
 * 其中在 son_2_father 中尚存在引用关系的不收集，
 * key为一个对象实例，值为一个set集合，其中包括所有其他引用了key的对象，set 中0个对象时，表示系统中没有别的对象对其引用。
 * 不过，有一些对象并没有被别的对象引用，但他仍然在运行程序中被使用，也不可回收，
 * 这些对象存在于 Runtime中的堆栈 Runtime->stack或者方法method的局部变量表中Runtime->localvar。
 *
 * 因此在收集时，收集线程会暂停正在执行中的java线程 ，然后比对这个线程中所有的 stack 和 localvar ,
 * 如果有对象正在栈或局部变量中，则标记为不可回收。
 *
 * 还有一个jdwp调试线程中的运行时对象不可回收。
 *
 * 检查完这几方面之后，那些 （1）引用对象为0个，（2）未被其他线程运行时使用的对象，将被回收。
 *
 * @return
 */
s32 garbage_collector_create() {

    _garbage_refer_set_pool = arraylist_create(4);
    _garbage_thread_pause = 1;
    _garbage_thread = jvm_alloc(sizeof(pthread_t));
    _garbageCond = PTHREAD_COND_INITIALIZER;
    pthread_cond_init(&_garbageCond, NULL);
    pthread_mutexattr_init(&_garbage_attr);
    pthread_mutexattr_settype(&_garbage_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&_garbage_lock, &_garbage_attr);
    pthread_create(_garbage_thread, NULL, collect_thread_run, NULL);
    return 0;
}

void garbage_collector_destory() {
    pthread_exit(_garbage_thread);
    jvm_free(_garbage_attr);
    jvm_free(_garbage_lock);
    jvm_free(_garbage_thread);
}

void recycle_bin_init(RecycleBin *bin) {
    bin->son_2_father = hashtable_create(DEFAULT_HASH_FUNC, DEFAULT_HASH_EQUALS_FUNC);
    bin->father_2_son = hashtable_create(DEFAULT_HASH_FUNC, DEFAULT_HASH_EQUALS_FUNC);

}

void recycle_bin_free(RecycleBin *bin) {
    hashtable_destory(bin->son_2_father);
    hashtable_destory(bin->father_2_son);

}

void *collect_thread_run(void *para) {
    while (!_garbage_thread_stop) {
        //_garbage_thread_pause=1;
        if (!_garbage_thread_pause) {
            garbage_collect();
        }
        threadSleep(GARBAGE_PERIOD_MS);
    }
    return NULL;
}

s32 garbage_thread_trylock() {
    return pthread_mutex_trylock(&_garbage_lock);
}

void garbage_thread_lock() {
    pthread_mutex_lock(&_garbage_lock);
}

void garbage_thread_unlock() {
    pthread_mutex_unlock(&_garbage_lock);
}

void garbage_thread_stop() {
    _garbage_thread_stop = 1;
}

void garbage_thread_wait() {
    pthread_cond_wait(&_garbageCond, &_garbage_lock);
}

void garbage_thread_notify() {
    pthread_cond_signal(&_garbageCond);
}

Hashset *_garbage_get_set() {
    if (!_garbage_refer_set_pool->length) {
        Hashset *set = hashset_create();
        return set;
    }
    return arraylist_pop_back(_garbage_refer_set_pool);
}

/**
 * 3 mode to mem or perfomence
 * @param set
 */
void _garbage_put_set(Hashset *set) {
    s32 mode = 0;
    switch (mode) {
        case 0:
            //no cache
            hashset_destory(set);
            break;
        case 1:
            //cache limited hashset
            if (_garbage_refer_set_pool->length > 100000) {
                hashset_destory(set);
                return;
            }
            hashset_clear(set);
            arraylist_append(_garbage_refer_set_pool, set);
            break;
        case 2:
            //it would be big mem use
            hashset_clear(set);
            arraylist_append(_garbage_refer_set_pool, set);
            break;
    }
}

//===================================================================
/**
 * 查找所有实例，如果发现没有被引用 set->length==0 时，也不在运行时runtime的 stack 和 局部变量表中
 * 去除掉此对象对其他对象的引用，并销毁对象
 *
 * @return
 */
s32 garbage_collect() {
    garbage_thread_lock();
#if _JVM_DEBUG_GARBAGE_DUMP
    //dump_refer();
#endif
    s32 x;
    //jvm_printf("thread set size:%d\n", thread_list->length);
    for (x = 0; x < thread_list->length; x++) {
        Runtime *runtime = (Runtime *) arraylist_get_value(thread_list, x);
        RecycleBin *bin = &runtime->threadInfo->reclcle_bin;

        HashtableIterator hti;
        hashtable_iterate(bin->son_2_father, &hti);
        //jvm_printf("hmap_t:%d\n", hashtable_num_entries(son_2_father));
        s64 obj_count = hashtable_num_entries(bin->son_2_father);
        s64 mem1 = heap_size;

        //把所有对象标记为可回收
        for (; hashtable_iter_has_more(&hti);) {
            MemoryBlock *mb = (MemoryBlock *) hashtable_iter_next_key(&hti);
            mb->garbage_mark = GARBAGE_MARK_NO_REFERED;
        }
        //让线程标记，小于0表示标记失败，则不继续回收
        //in this sub proc ,in wait maybe other thread insert new obj into son_2_father ,
        // so new inserted obj must be garbage_mark==GARBAGE_MARK_UNDEF  ,otherwise new obj would be collected.
        if (garbage_mark_by_threads(runtime) < 0) {
            continue;
        }

        //如果没被其他对象引用set->length==0，也没有被标记为引用，则回收掉
        s32 i = 0;
        hashtable_iterate(bin->son_2_father, &hti);
        for (; hashtable_iter_has_more(&hti);) {

            HashtableKey k = hashtable_iter_next_key(&hti);
            MemoryBlock *mb = (MemoryBlock *) k;
            Hashset *v = (Hashset *) hashtable_get(bin->son_2_father, k);

            if (v != HASH_NULL) {
                if (v->entries == 0 && mb->garbage_mark == GARBAGE_MARK_NO_REFERED) {
                    garbage_destory_memobj(runtime, k);
                    i++;
                }
            }
        }
//    jvm_printf("garbage cllected OBJ: %lld -> %lld    MEM : %lld -> %lld\n",
//           obj_count, hashtable_num_entries(son_2_father), mem1, heap_size);

        if (_garbage_count++ % 5 == 0) {//每n秒resize一次
            hashtable_remove(bin->son_2_father, NULL, 1);
            hashtable_remove(bin->father_2_son, NULL, 1);
        }
    }
    garbage_thread_unlock();

    return 0;
}

/**
 * destory a instance
 * @param k
 */
void garbage_destory_memobj(Runtime *runtime, __refer k) {
    RecycleBin *bin = &runtime->threadInfo->reclcle_bin;
    garbage_derefer_all(k, runtime);

#if _JVM_DEBUG_GARBAGE_DUMP
    //    Utf8String *pus = utf8_create();
    //    getMemBlockName(k, pus);
    //    jvm_printf("garbage collect instance  %s [%x]\n", utf8_cstr(pus), k);
    //    utf8_destory(pus);
#endif
    Hashset *v = (Hashset *) hashtable_get(bin->son_2_father, k);
    _garbage_put_set(v);
    hashtable_remove(bin->son_2_father, k, 0);
    instance_destory(k, runtime);

}

/**
 * 各个线程把自己还需要使用的对象从引用列表中移除，表示不能回收
 * @param son
 * @return
 */
s32 garbage_mark_by_threads(Runtime *runtime) {
//    s32 i;
//    //jvm_printf("thread set size:%d\n", thread_list->length);
//    for (i = 0; i < thread_list->length; i++) {
//        Runtime *runtime = (Runtime *) arraylist_get_value(thread_list, i);
    /**
     *  回收线程 进行收集，先暂停线程，收集完之后恢复java工作线程
     *
     */
    if (runtime->threadInfo->thread_status != THREAD_STATUS_ZOMBIE) {
        jthread_suspend(runtime);
        //等一段时间，如果线程进入了暂停状态，则开始标记，如果没进入，则放弃本次回收
        s64 start = currentTimeMillis();
        while ((currentTimeMillis() - start) < 1000) {
            if (runtime->threadInfo->is_suspend) {
                break;
            }
        }
        if (!runtime->threadInfo->is_suspend) {
            return -1;
        }
        garbage_mark_refered_obj(runtime);
        jthread_resume(runtime);
    }
    //jvm_printf("thread marked refered , [%llx]\n", (s64) (long) runtime->threadInfo->jthread);
//    }
//    //调试线程
//    if (java_debug) {
//        Runtime *runtime = jdwpserver.runtime;
//        jthread_suspend(runtime);
//        garbage_mark_refered_obj(runtime);
//        jthread_resume(runtime);
//    }
    return 0;
}

/**
 * 判定某个对象是否被所有线程的runtime引用
 * 引用有两种情况：
 * 一种是被其他实例所引用，会putfield或putstatic
 * 另一种是被运行时的栈或局部变量所引用，
 * 这两种情况下，对象是不能被释放的
 *
 * @param son
 * @param jthread
 * @return
 */


s32 garbage_mark_refered_obj(Runtime *pruntime) {
    s32 i;
    StackEntry entry;
    Runtime *runtime = pruntime;
    StackFrame *stack = runtime->stack;
    for (i = 0; i < stack->size; i++) {
        peek_entry(stack, &entry, i);
        if (is_ref(&entry)) {
            __refer ref = entry_2_refer(&entry);
            if (ref) {
                //jvm_printf("mark:[%llx]   ",(s64)(long)ref);
                ((MemoryBlock *) ref)->garbage_mark = GARBAGE_MARK_REFERED;
            }
        }
    }
    while (runtime) {
        for (i = 0; i < runtime->localvar_count; i++) {
            __refer ref = (runtime->localVariables + i)->refer;
            if (ref) {
                //jvm_printf("mark:[%llx]   ",(s64)(long)ref);
                ((MemoryBlock *) ref)->garbage_mark = GARBAGE_MARK_REFERED;
            }
        }
        runtime = runtime->son;
    }
    //jvm_printf("[%llx] notified\n", (s64) (long) pruntime->threadInfo->jthread);
    return 0;
}


void getMemBlockName(void *memblock, Utf8String *name) {

    MemoryBlock *mb = (MemoryBlock *) memblock;
    if (!mb) {
        utf8_append_c(name, "NULL");
    } else {
        switch (mb->type) {
            case MEM_TYPE_CLASS: {
                Class *clazz = (Class *) mb;
                utf8_append(name, clazz->name);
                break;
            }
            case MEM_TYPE_INS: {
                Instance *ins = (Instance *) mb;
                utf8_append_c(name, "L");
                utf8_append(name, ins->mb.clazz->name);
                utf8_append_c(name, ";");
                break;
            }
            case MEM_TYPE_ARR: {
                Instance *arr = (Instance *) mb;
                //jvm_printf("Array{%d}", data_type_bytes[arr->arr_data_type]);
                utf8_append_c(name, "Array{");
                utf8_append(name, arr->mb.clazz->name);//'0' + data_type_bytes[arr->arr_data_type]);
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
void dump_refer(RecycleBin *bin) {
    //jvm_printf("%d\n",sizeof(struct _Hashtable));
    HashtableIterator hti;
    hashtable_iterate(bin->son_2_father, &hti);
    jvm_printf("=========================son <- father :%d\n", hashtable_num_entries(bin->son_2_father));
    for (; hashtable_iter_has_more(&hti);) {

        HashtableKey k = hashtable_iter_next_key(&hti);
        Utf8String *name = utf8_create();
        getMemBlockName(k, name);
        jvm_printf("%s[%llx] <-{", utf8_cstr(name), (s64) (long) k);
        utf8_destory(name);
        Hashset *set = (Hashset *) hashtable_get(bin->son_2_father, k);
        if (set != HASH_NULL) {
            HashsetIterator hti;
            hashset_iterate(set, &hti);

            for (; hashset_iter_has_more(&hti);) {
                HashsetKey key = hashset_iter_next_key(&hti);
                if (key) {
                    MemoryBlock *parent = (MemoryBlock *) key;
                    Utf8String *name = utf8_create();
                    getMemBlockName(parent, name);
                    jvm_printf("%s[%llx]; ", utf8_cstr(name), (s64) (long) parent);
                    utf8_destory(name);
                }
            }
        }
        jvm_printf("}\n");
    }

    hashtable_iterate(bin->father_2_son, &hti);
    jvm_printf("=========================father -> son :%d\n", hashtable_num_entries(bin->father_2_son));
    for (; hashtable_iter_has_more(&hti);) {

        HashtableKey k = hashtable_iter_next_key(&hti);
        Utf8String *name = utf8_create();
        getMemBlockName(k, name);
        jvm_printf("%s[%llx] ->{", utf8_cstr(name), (s64) (long) k);
        utf8_destory(name);
        Hashset *set = (Hashset *) hashtable_get(bin->father_2_son, k);
        if (set != HASH_NULL) {
            HashsetIterator hti;
            hashset_iterate(set, &hti);

            for (; hashset_iter_has_more(&hti);) {
                HashsetKey key = hashset_iter_next_key(&hti);
                if (key) {
                    Instance *son = (Instance *) key;
                    Utf8String *name = utf8_create();
                    getMemBlockName(son, name);
                    jvm_printf("%s[%llx]; ", utf8_cstr(name), (s64) (long) son);
                    utf8_destory(name);
                }
            }
        }
        jvm_printf("}\n");
    }
}

/**
 *    建立引用列表
 *    1.putfield 时需要创建引用，并把原来的引用从列表删除
 *    2. aastore 时需要创建引用，并把原来的引用从列表删除
 *    3. new 新对象时创建引用，其父为 Runtime 对象，因为此对象可能还在栈空间，Runtime 销毁时，删除对其的引用
 *    4. newarray 新数组创建引用，其父为 Runtime 对象，因为此对象可能还在栈空间，Runtime 销毁时，删除对其的引用
 *
 * @param sonPtr
 * @param parentPtr
 * @return
 */

void *garbage_refer(__refer sonPtr, __refer parentPtr, Runtime *runtime) {
    if (sonPtr == NULL)return sonPtr;
    if (sonPtr == parentPtr) {
        return sonPtr;
    }
    garbage_thread_lock();
    RecycleBin *bin = &runtime->threadInfo->reclcle_bin;
#if _JVM_DEBUG_GARBAGE_DUMP
    Utf8String *pus = utf8_create();
    Utf8String *sus = utf8_create();
    getMemBlockName(parentPtr, pus);
    getMemBlockName(sonPtr, sus);
    jvm_printf("+: %s[%x] <- %s[%x]\n", utf8_cstr(sus), sonPtr, utf8_cstr(pus),
               parentPtr);
    utf8_destory(pus);
    utf8_destory(sus);
#endif
    //放入子引父
    Hashset *set = (Hashset *) hashtable_get(bin->son_2_father, sonPtr);
    if (set == HASH_NULL) {
        set = _garbage_get_set();
        hashtable_put(bin->son_2_father, sonPtr, set);
    }
    if (parentPtr) {
        hashset_put(set, parentPtr);
        //放入父引子
        set = hashtable_get(bin->father_2_son, parentPtr);
        if (set == HASH_NULL) {
            set = _garbage_get_set();
            hashtable_put(bin->father_2_son, parentPtr, set);
        }
        hashset_put(set, sonPtr);
    }
    garbage_thread_unlock();
    return sonPtr;
}

void garbage_derefer(__refer sonPtr, __refer parentPtr, Runtime *runtime) {
    garbage_thread_lock();
    RecycleBin *bin = &runtime->threadInfo->reclcle_bin;
#if _JVM_DEBUG_GARBAGE_DUMP
    Utf8String *pus = utf8_create();
    Utf8String *sus = utf8_create();
    getMemBlockName(parentPtr, pus);
    getMemBlockName(sonPtr, sus);
    jvm_printf("-: %s[%x] <- %s[%x]\n", utf8_cstr(((Instance *) sonPtr)->mb.clazz->name), sonPtr, utf8_cstr(pus),
               parentPtr);
    utf8_destory(sus);
    utf8_destory(pus);
#endif
    Hashset *set;
    if (sonPtr && parentPtr) {
        //移除子引父
        set = hashtable_get(bin->son_2_father, sonPtr);
        if (set != HASH_NULL) {
            hashset_remove(set, parentPtr, 0);
        }


        //移除父引子
        set = hashtable_get(bin->father_2_son, parentPtr);
        if (set != HASH_NULL) {
            hashset_remove(set, sonPtr, 0);
        }
    }
    garbage_thread_unlock();
}

void garbage_derefer_all(__refer parentPtr, Runtime *runtime) {
    garbage_thread_lock();
    RecycleBin *bin = &runtime->threadInfo->reclcle_bin;
#if _JVM_DEBUG_GARBAGE_DUMP
    Utf8String *us = utf8_create();
    getMemBlockName(parentPtr, us);
    jvm_printf("X:  %s[%x]\n", utf8_cstr(us), parentPtr);
    utf8_destory(us);
#endif
    Hashset *set;
    //移除父引用的所有子
    set = hashtable_get(bin->father_2_son, parentPtr);
    if (set) {
        HashsetIterator hti;
        hashset_iterate(set, &hti);

        for (; hashset_iter_has_more(&hti);) {
            HashsetKey key = hashset_iter_next_key(&hti);
            if (key) {
                garbage_derefer(key, parentPtr, runtime);
            }
        }
        _garbage_put_set(set);
    }
    hashtable_remove(bin->father_2_son, parentPtr, 0);
    garbage_thread_unlock();

}

/**
 * 在分配的内存块前面加4个字节用于存放此块内存的长度
 * @param size
 * @return
 */
void *jvm_alloc(u32 size) {
    if (!size)return NULL;
//    if (size > 5000) {
//        int debug = 1;
//    }
    size += 4;
    void *ptr = malloc(size);
    if (ptr) {
        memset(ptr, 0, size);
        heap_size += size;
        setFieldInt(ptr, size);
        return ptr + 4;
    }
    return NULL;
}

s32 jvm_free(void *ptr) {
    if (ptr) {
        s32 size = getFieldInt(ptr - 4);
        heap_size -= size;
//        if (size == 176) {
//            int debug = 1;
//        }
        free(ptr - 4);
        return size;
    }
    return 0;
}

void *jvm_realloc(void *pPtr, u32 size) {
    if (!pPtr)return NULL;
    if (!size)return NULL;
    size += 4;
    heap_size -= getFieldInt(pPtr - 4);
    void *ptr = realloc(pPtr - 4, size);
    if (ptr) {
        heap_size += size;
        setFieldInt(ptr, size);
        return ptr + 4;
    }
    return NULL;
}
/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2018 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: wxxiong6@gmail.com                                           |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_snowflake.h"
#include <sys/time.h>
#include <sched.h>
#include <sys/ipc.h>
#include <sys/shm.h>


static uint8_t ncpu = 1;
static uint32_t pid = 0;

/* True global resources - no need for thread safety here */
static int le_snowflake;
static snowflake snowf;
static sf_shmtx_t *mtx;
static int shm_id;

zend_class_entry snowflake_ce;

/* {{{ PHP_INI  */
 

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("snowflake.worker_id",     "1",     PHP_INI_SYSTEM, OnUpdateLongGEZero, worker_id,     zend_snowflake_globals, snowflake_globals)
    STD_PHP_INI_ENTRY("snowflake.region_id",     "1",     PHP_INI_SYSTEM, OnUpdateLongGEZero, region_id,     zend_snowflake_globals, snowflake_globals)
    //1576080000000 2019-12-12
	STD_PHP_INI_ENTRY("snowflake.epoch",         "1576080000000",         PHP_INI_SYSTEM, OnUpdateLongGEZero, epoch,         zend_snowflake_globals, snowflake_globals)
	STD_PHP_INI_ENTRY("snowflake.time_bits",     "41",     PHP_INI_SYSTEM, OnUpdateLongGEZero, time_bits,     zend_snowflake_globals, snowflake_globals)
    STD_PHP_INI_ENTRY("snowflake.region_bits",   "5",      PHP_INI_SYSTEM, OnUpdateLongGEZero, region_bits,   zend_snowflake_globals, snowflake_globals)
    STD_PHP_INI_ENTRY("snowflake.worker_bits",   "5",      PHP_INI_SYSTEM, OnUpdateLongGEZero, worker_bits,   zend_snowflake_globals, snowflake_globals)
	STD_PHP_INI_ENTRY("snowflake.sequence_bits", "12",     PHP_INI_SYSTEM, OnUpdateLongGEZero, sequence_bits, zend_snowflake_globals, snowflake_globals)
PHP_INI_END()

/* }}} */

static inline uint64_t timestamp_gen()
{
    struct  timeval    tv;
    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec) * 1000 + ((uint64_t)tv.tv_usec) / 1000; 
} 

static uint64_t snowflake_id(snowflake *sf) 
{
    uint64_t now = timestamp_gen();
    
    if (EXPECTED(now == mtx->last_time))
    {
        mtx->seq = (mtx->seq+1) & sf->seq_mask;
        if (mtx->seq == 0)
        {
            while (now <= mtx->last_time)
            {
                now = timestamp_gen();
            }
        }
    }  
    else if (now > mtx->last_time) 
    {
        mtx->seq = 0;
    }
    else 
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "last_time in the range of 0, %lld, last_time:%lld", now, mtx->last_time);
    }
    mtx->last_time = now;
    return ((now-SF_G(epoch)) << sf->time_shift) 
            | (SF_G(region_id) << sf->region_shift) 
            | (SF_G(worker_id) << sf->worker_shift) 
            | (mtx->seq);
}

static int snowflake_init(snowflake *sf) 
{
    uint8_t region_id = SF_G(region_id);
    uint8_t max_region_id = (-1L << SF_G(region_bits)) ^ -1L;
    if (region_id < 0 || region_id > max_region_id)
    {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Region ID must be in the range : 0-%d", max_region_id);
        return FAILURE;
    }
    
    uint8_t worker_id = SF_G(worker_id);
    uint8_t max_worker_id = (-1L << SF_G(worker_bits)) ^ -1L;
    if (worker_id < 0 || worker_id > max_worker_id)
    {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Worker ID must be in the range: 0-%d", max_worker_id);
        return FAILURE;
    }

    sf->time_shift   = SF_G(region_bits) + SF_G(worker_bits) + SF_G(sequence_bits);
    sf->region_shift = SF_G(worker_bits) + SF_G(sequence_bits);
    sf->worker_shift = SF_G(sequence_bits);
    sf->seq_mask     = (-1L << SF_G(sequence_bits)) ^ -1L;

    return SUCCESS;
}

static int trylock(sf_shmtx_t *mtx)
{
    return (mtx->lock == 0 && __sync_bool_compare_and_swap(&(mtx->lock), 0, pid)); 
}

static void spinlock(sf_shmtx_t *mtx)
{
    uint32_t  i, n;
    for ( ;; ) {
        if (mtx->lock == 0 && __sync_bool_compare_and_swap(&(mtx->lock), 0, pid)) {
            return;
        }
        if (ncpu > 1) { // ncpu CPU个数
            for (n = 1; n < mtx->spin; n <<= 1) {
                for (i = 0; i < n; i++) {
                    __asm__ __volatile__ ("pause");
                }
                if (mtx->lock == 0 && __sync_bool_compare_and_swap(&(mtx->lock), 0, pid)) {
                    return;
                }
            }
        }
        sched_yield(); // 只旋一个周期，没有获取到锁，退出CPU控制权
    }   
}

static void spinlock_unlock(sf_shmtx_t *mtx)
{
    __sync_bool_compare_and_swap(&(mtx->lock), pid, 0);
}

static int sf_shmtx_shutdown(sf_shmtx_t **mtx)
{
    if ((*mtx)->lock != NULL && (*mtx)->lock != 0) 
    {
        spinlock_unlock(*mtx);
    }
    shmdt(*mtx);
    shmctl(shm_id, IPC_RMID, 0) ;
    
    return SUCCESS;
}

static int sf_shmtx_create(sf_shmtx_t **mtx) 
{
    shm_id = shmget(IPCKEY , sizeof(sf_shmtx_t), 0640);
    if (shm_id != -1)
    {
        *mtx = (sf_shmtx_t *)shmat(shm_id, NULL, 0);
        sf_shmtx_shutdown(mtx);
    } 

    shm_id = shmget(IPCKEY, sizeof(sf_shmtx_t), 0640 | IPC_CREAT | IPC_EXCL);
    if (shm_id == -1) 
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Init the shared memory[%dB] failed [%d:%s]!", sizeof(sf_shmtx_t), errno, strerror(errno));
        return FAILURE;
    } 
    else
    {
        *mtx = (sf_shmtx_t *) shmat(shm_id, NULL, 0);
    }
    
  
    (*mtx)->lock = 0;
    (*mtx)->spin = 2048;
    (*mtx)->last_time = 0;
    (*mtx)->seq = 0;
    return SUCCESS;
}


/* {{{ php_snowflake_init_globals
 */

static void php_snowflake_init_globals(zend_snowflake_globals *snowflake_globals)
{
   snowflake_globals->region_id     = 1;
   snowflake_globals->worker_id     = 1;
   snowflake_globals->epoch         = 1576080000000;
   snowflake_globals->region_bits   = 41;
   snowflake_globals->worker_bits   = 5;
   snowflake_globals->sequence_bits = 5;
   snowflake_globals->time_bits     = 12;
}

/* }}} */


PHP_METHOD(snowflake, getId)
{
	if (zend_parse_parameters_none() == FAILURE) 
    {
		return;
	}
    spinlock(mtx);
    uint64_t id = snowflake_id(&snowf);
    spinlock_unlock(mtx);
    if (id) 
    {
        RETURN_LONG(id);
    } 
    else 
    {
        RETURN_BOOL(0);
    }
}


/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(snowflake)
{
	UNREGISTER_INI_ENTRIES();
    sf_shmtx_shutdown(&mtx);
   
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(snowflake)
{

#if defined(COMPILE_DL_SNOWFLAKE) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
    if (pid == 0) 
    {
        pid = (uint32_t)getpid();
    }
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(snowflake)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(snowflake)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "snowflake support", "enabled");
    php_info_print_table_row(2, "Version", PHP_SNOWFLAKE_VERSION);
	php_info_print_table_end();
    
	/* Remove comments if you have entries in php.ini */
	DISPLAY_INI_ENTRIES();
	
}
/* }}} */

/* {{{ arginfo
 */
ZEND_BEGIN_ARG_INFO(arginfo_snowflake_void, 0)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ snowflake_functions[]
 *
 * Every user visible function must have an entry in snowflake_functions[].
 */
const zend_function_entry snowflake_functions[] = {
	PHP_FE_END	/* Must be the last line in snowflake_functions[] */
};
/* }}} */

/* {{{ snowflake_method[]
 *
 * Every user visible function must have an entry in snowflake_functions[].
 */
static const zend_function_entry snowflake_methods[] = { 
	PHP_ME(snowflake, getId,        arginfo_snowflake_void,      ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_FE_END
};
/* }}} */


/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(snowflake)
{
    ZEND_INIT_MODULE_GLOBALS(snowflake, php_snowflake_init_globals, NULL);
	INIT_CLASS_ENTRY(snowflake_ce, "snowflake", snowflake_methods); //注册类及类方法
	REGISTER_INI_ENTRIES();
    zend_register_internal_class(&snowflake_ce);
    
    if (sf_shmtx_create(&mtx) == FAILURE) 
    {
        return FAILURE;
    }
    if (snowflake_init(&snowf) == FAILURE) 
    {
        return FAILURE;
    }
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	return SUCCESS;
}
/* }}} */

/* {{{ snowflake_module_entry
 */
zend_module_entry snowflake_module_entry = {
	STANDARD_MODULE_HEADER,
	"snowflake",
	snowflake_functions,
	PHP_MINIT(snowflake),
	PHP_MSHUTDOWN(snowflake),
	PHP_RINIT(snowflake),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(snowflake),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(snowflake),
	PHP_SNOWFLAKE_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_SNOWFLAKE
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(snowflake)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */

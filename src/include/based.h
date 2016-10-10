#ifndef	BASED_H
#define	BASED_H
#if BASED_MODE
#ifdef __cplusplus
extern "C" {
#endif

#ifndef	__STDC_LIMIT_MACROS
#define	__STDC_LIMIT_MACROS
#endif

#include <sys/types.h>
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdio.h>
#include <libpmemobj.h>


#define	PMEMOBJ_NUM_TB_TYPES ((unsigned)1024)
/*#if BASED_MODE
extern int64_t based_p;
extern uint64_t based_id;
#endif*/
 /*
 * Typed persistenceI
 */
// check how are they doing it for TOID
#define	TB(t)\
union _tb_##t##_tb

#ifdef	__cplusplus
#define	_TB_CONSTR(t)\
_tb_##t##_tb()\
{ }
#else
#define	_TB_CONSTR(t)
#endif
 // let's get back to this to check whether we will need something like this or not
//#define	PI_NULL	((persistenceI) {0})
// 
#define	B_NULL	((long) {0})
#define	TB_NULL(t)	((TB(t))B_NULL)
// If the pointer does not point to anything then the object_offset should be zero
#define	B_IS_NULL(o)	((o).object_off == 0)
// okay pmemoid is being used - so we will have to add an extra access from our struct to pmemoid and then further on or just check the PMEMoid object_address?
/*
#define	PI_EQUALS(lhs, rhs)\
((lhs).object_address.off == (rhs).pi.object_address.off)
*/
/*
 * Type safety macros
 */

#define	TB_ASSIGN(o, value) (\
{	(o).object_off = value;\
	(o);\
})
/*
#define	TPI_EQUALS(lhs, rhs)\
((lhs).pi.object_address.off == (rhs).pi.object_address.off &&\
	(lhs).pi.object_address.pool_uuid_lo == (rhs).pi.object_address.pool_uuid_lo)
*/
/* type number of root object */
#define	_tb_struct
#define	_tb_union
#define	_tb_enum



/*
 * This will cause a compile-time error for typed OID's declared
 * with a type number exceeding the limit.
 */
 
 // will have to search for and incorporate the PMEMOBJ_NUM_TPI_TYPES so that there is a unique id associated with TPI type as well
 // but this can be done later
#define	_TB_TYPE_NUM(i) ((i) < PMEMOBJ_NUM_TB_TYPES ? (i) : UINT64_MAX)

/*
 * Declaration of typed OID
 */
#define	_TB_DECLARE(t, i)\
typedef uint8_t _tb_##t##_tb_type_num[(i)];\
TB(t)\
{\
	t *_type;\
	_tb_##t##_tb_type_num *_type_num;\
	long object_off;\
}

/*
 * Declaration of typed persistentI of an object
 */
#define	TB_DECLARE(t, i) _TB_DECLARE(t, _TB_TYPE_NUM(i))

/*
 * Declaration of typed persistenceI of a root object -- no need for this
 */
#define	TB_DECLARE_ROOT(t) _TB_DECLARE(t, POBJ_ROOT_TYPE_NUM)

/*
 * Type number of specified type
 */
#define	TB_TYPE_NUM(t) (sizeof (_tb_##t##_tb_type_num))

/*
 * Type number of object read from typed OID
 */
#define	TB_TYPE_NUM_OF(o) (sizeof (*(o)._type_num))

/*
 * NULL check
 */
#define	TB_IS_NULL(o)((o).object_off == 0)

/*
 * Validates whether type number stored in typed OID is the same
 * as type number stored in object's metadata
 */
 // check where to incorp for TPI
#define	TB_VALID(o) (TB_TYPE_NUM_OF(o) == pmemobj_type_num((o)))

/*
 * Checks whether the object is of a given type
 */
#define	B_INSTANCEOF(o, t) (TB_TYPE_NUM(t) == pmemobj_type_num(o))


/*
 * Declaration of typed OID inside layout declaration
 */
 // check if any extra support if required for this
#define	POBJ_LAYOUT_TB(name, t)\
TB_DECLARE(t, (__COUNTER__ - _POBJ_LAYOUT_REF(name)));

/*
 * Declaration of typed OID of root inside layout declaration
 */
// check if any extra support if required for this
#define	TB_POBJ_LAYOUT_ROOT(name, t)\
TB_DECLARE_ROOT(t);

//based_p = pop;\
//  based_id = _pobj_ret.pool_uuid_lo;
#define POBJ_ROOT_BASED(pop, t) (\
{ PMEMoid _pobj_ret = pmemobj_root((pop), sizeof (t));\
based_p = pop;\
based_id = _pobj_ret.pool_uuid_lo;\
(t *)(based_p+_pobj_ret.off); })

// based_id = o.pool_uuid_lo;\


#define	POBJ_ALLOC_BASED(pop, t, size, constr, arg) (\
{\
PMEMoid o = OID_NULL;\
pmemobj_alloc((pop), &o, (size), TB_TYPE_NUM(t), (constr), (arg));\
((t *)(o.off));\
})


/*
#define	POBJ_ALLOC_BASED(pop, t, size, constr, arg) (\
{\
PMEMoid o = OID_NULL;\
pmemobj_alloc((pop), &o, (size), TB_TYPE_NUM(t), (constr), (arg));\
((t *)(o.off));\
})
*/

#define POBJ_REALLOC_BASED(pop, prev, t, size) (\
{ PMEMoid oid = OID_NULL;\
oid.pool_uuid_lo = based_id;\
oid.off = (((unsigned long long)prev)) ;\
pmemobj_realloc((pop), &oid, (size), TB_TYPE_NUM(t));\
((t *)(oid.off|(0x4000000000000000ULL)));\
})

#define POBJ_FREE_BASED(o)(\
{PMEMoid oid = OID_NULL;\
oid.pool_uuid_lo = based_id;\
oid.off = (((intptr_t)o));\
pmemobj_free(&oid);})


#define read_based(o)({\
o ? (typeof(*o) *)(based_p + (intptr_t)o) : 0; }) // (((intptr_t)o)&0x1fffffffffffffffLL)) : 0;})


/*
#define read_based(o)({\
o ? (typeof(*o) *)(based_p + ((intptr_t)o)) : 0;})
*/

#define write_based(o, offset)(o = offset)
#ifdef __cplusplus
}
#endif
#endif
#endif	/* based.h */

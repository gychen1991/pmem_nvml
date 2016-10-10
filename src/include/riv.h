
#ifndef	RIV_H
#define	RIV_H

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

#define WRIV_OFFSET(riv, offset) (\
{riv = (riv|offset);})
#define WRIV_RID(riv, rid) (riv = ((riv)|(((unsigned long long)rid)<<(l3))))


#define RIV_OFFSET(riv) ({unsigned long long riv_1 = (uintptr_t)riv;\
((riv_1)&generateMask(l3));})
#define RIV_RID(riv) ((((uintptr_t)riv)&(~generateMask(l3)))>>(l3))

#define	PMEMOBJ_NUM_TRIV_TYPES ((unsigned)1024)


// check how are they doing it for TOID
#define	TRIV(t)\
union _triv_##t##_triv

#ifdef	__cplusplus
#define	_TRIV_CONSTR(t)\
_triv_##t##_triv()\
{ }
#else
#define	_TRIV_CONSTR(t)
#endif
 // let's get back to this to check whether we will need something like this or not
//#define	PI_NULL	((persistenceI) {0})
// 
#define	RIV_NULL	((unsigned long long)0ULL)
#define	TRIV_NULL(t)	((TRIV(t))RIV_NULL)
// If the pointer does not point to anything then the object_offset should be zero
#define	RIV_IS_NULL(o)	((o).riv == 0)
// okay pmemoid is being used - so we will have to add an extra access from our struct to pmemoid and then further on or just check the PMEMoid object_address?
/*
#define	PI_EQUALS(lhs, rhs)\
((lhs).object_address.off == (rhs).pi.object_address.off)
*/
/*
 * Type safety macros
 */

#define	TRIV_ASSIGN(o, value) (\
{	(o).riv = value;\
	(o);\
})
/*
#define	TPI_EQUALS(lhs, rhs)\
((lhs).pi.object_address.off == (rhs).pi.object_address.off &&\
	(lhs).pi.object_address.pool_uuid_lo == (rhs).pi.object_address.pool_uuid_lo)
*/
/* type number of root object */
#define	_triv_struct
#define	_triv_union
#define	_triv_enum



/*
 * This will cause a compile-time error for typed OID's declared
 * with a type number exceeding the limit.
 */
 

#define	_TRIV_TYPE_NUM(i) ((i) < PMEMOBJ_NUM_TRIV_TYPES ? (i) : UINT64_MAX)

/*
 * Declaration of typed OID
 */
#define	_TRIV_DECLARE(t, i)\
typedef uint8_t _triv_##t##_triv_type_num[(i)];\
TRIV(t)\
{\
	t *_type;\
	_triv_##t##_triv_type_num *_type_num;\
	unsigned long long riv;\
}

/*
 * Declaration of typed persistentI of an object
 */
#define	TRIV_DECLARE(t, i) _TRIV_DECLARE(t, _TRIV_TYPE_NUM(i))

/*
 * Declaration of typed persistenceI of a root object -- no need for this
 */
#define	TRIV_DECLARE_ROOT(t) _TRIV_DECLARE(t, POBJ_ROOT_TYPE_NUM)

/*
 * Type number of specified type
 */
#define	TRIV_TYPE_NUM(t) (sizeof (_triv_##t##_triv_type_num))

/*
 * Type number of object read from typed OID
 */
#define	TRIV_TYPE_NUM_OF(o) (sizeof (*(o)._type_num))

/*
 * NULL check
 */
#define	TRIV_IS_NULL(o)((o).riv == 0)

/*
 * Validates whether type number stored in typed OID is the same
 * as type number stored in object's metadata
 */
 // check where to incorp for TPI
#define	TRIV_VALID(o) (TRIV_TYPE_NUM_OF(o) == pmemobj_type_num((o)))

/*
 * Checks whether the object is of a given type
 */
#define	RIV_INSTANCEOF(o, t) (TRIV_TYPE_NUM(t) == pmemobj_type_num(o))


/*
 * Declaration of typed OID inside layout declaration
 */
 // check if any extra support if required for this
#define	POBJ_LAYOUT_TRIV(name, t)\
TRIV_DECLARE(t, (__COUNTER__ - _POBJ_LAYOUT_REF(name)));

/*
 * Declaration of typed OID of root inside layout declaration
 */
// check if any extra support if required for this
#define	TRIV_POBJ_LAYOUT_ROOT(name, t)\
TRIV_DECLARE(t, (__COUNTER__ - _POBJ_LAYOUT_REF(name)));


#define	POBJ_ALLOC_TRIV(pop, t, size, constr, arg) (\
{ unsigned long long init = 0ULL; unsigned long long *riv = &init;\
PMEMoid o = OID_NULL;\
int outcome = 0;\
outcome = pmemobj_alloc((pop), &o, riv, (size), TRIV_TYPE_NUM(t), (constr), (arg));\
(((t *)((*riv)|0x2000000000000000ULL)));})


/*#define	POBJ_ALLOC_TRIV(pop, t, size, constr, arg) (\
{ unsigned long long riv = 0;\
PMEMoid o = OID_NULL;\
pmemobj_alloc((pop), &o, &riv, (size), TRIV_TYPE_NUM(t), (constr), (arg));\
(((t *)(riv)));})
*/


// for allocating the root object
#define POBJ_ROOT_TRIV(pop, t) (\
{ unsigned long long riv = (pmemobj_root((pop), sizeof (t)));\
((t *)(riv));})

#define POBJ_FREE_TRIV(p_del)(\
{PMEMoid variable = OID_NULL;\
variable.pool_uuid_lo = RIV_RID((((unsigned long long)p_del)&0x1fffffffffffffffULL));\
variable.off = RIV_OFFSET((((unsigned long long)p_del)&0x1fffffffffffffffULL));\
pmemobj_free(&variable);\
})


#define R_RIV(riv)(\
{	 (typeof(*riv) *)pmemobj_direct_riv(riv);\
})

/*
#define R_RIV(riv)(\
{	pmemobj_direct(actual_value);\
	riv ? (typeof(*riv) *)((((unsigned long long)getBase(RIV_RID(actual_value)))|(1ULL<<46))+RIV_OFFSET(actual_value)) : 0;})
*/	
	
#define POBJ_REALLOC_TRIV(pop, riv, t, size) (\
{ PMEMoid oid = OID_NULL;\
oid.pool_uuid_lo = RIV_RID((((unsigned long long)riv)&0x1fffffffffffffffULL));\
oid.off = RIV_OFFSET((((unsigned long long)riv)&0x1fffffffffffffffULL));\
pmemobj_realloc((pop), &oid, (size), TRIV_TYPE_NUM(t));\
unsigned long long r_riv = 0;\
WRIV_RID(r_riv, oid.pool_uuid_lo);\
WRIV_OFFSET(r_riv, oid.off);\
(t *)(r_riv|0x2000000000000000ULL);\
})

// 

#define convert_to_riv(normal_addr)(\
{\
unsigned long long a_mask = generateMask(l3);\
unsigned long long offset = ((uintptr_t)normal_addr)&a_mask;\
unsigned long long base = ((uintptr_t)normal_addr)&(~a_mask);\
unsigned long long rid = 0ULL;\
rid = base ? getRID(base): 0ULL;\
unsigned long long riv =  0ULL;\
WRIV_RID(riv, rid);\
WRIV_OFFSET(riv, offset);\
normal_addr ? (uintptr_t)(riv|0x2000000000000000ULL):0;\
})

#ifdef __cplusplus
}
#endif
#endif	/* riv.h */

#ifndef	OFFHOLDER_H
#define	OFFHOLDER_H

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


#define	PMEMOBJ_NUM_TPI_TYPES ((unsigned)1024)

 /*
 * Typed persistenceI
 */

#define	TPI(t)\
union _tpi_##t##_tpi

#ifdef	__cplusplus
#define	_TPI_CONSTR(t)\
_tpi_##t##_tpi()\
{ }
#else
#define	_TPI_CONSTR(t)
#endif
 // let's get back to this to check whether we will need something like this or not
//#define	PI_NULL	((persistenceI) {0})
// 
#define	PI_NULL	((long) {0})
#define	TPI_NULL(t)	((TPI(t))PI_NULL)
// If the pointer does not point to anything then the object_offset should be zero
#define	PI_IS_NULL(o)	((o).object_off == 0)
// okay pmemoid is being used - so we will have to add an extra access from our struct to pmemoid and then further on or just check the PMEMoid object_address?
/*
#define	PI_EQUALS(lhs, rhs)\
((lhs).object_address.off == (rhs).pi.object_address.off)
*/
/*
 * Type safety macros
 */

#define	TPI_ASSIGN(o, value) (\
{	(o).object_off = value;\
	(o);\
})
/*
#define	TPI_EQUALS(lhs, rhs)\
((lhs).pi.object_address.off == (rhs).pi.object_address.off &&\
	(lhs).pi.object_address.pool_uuid_lo == (rhs).pi.object_address.pool_uuid_lo)
*/
/* type number of root object */
#define	_tpi_struct
#define	_tpi_union
#define	_tpi_enum



/*
 * This will cause a compile-time error for typed OID's declared
 * with a type number exceeding the limit.
 */
 
 // will have to search for and incorporate the PMEMOBJ_NUM_TPI_TYPES so that there is a unique id associated with TPI type as well
 // but this can be done later
#define	_TPI_TYPE_NUM(i) ((i) < PMEMOBJ_NUM_TPI_TYPES ? (i) : UINT64_MAX)

/*
 * Declaration of typed OID
 */
#define	_TPI_DECLARE(t, i)\
typedef uint8_t _tpi_##t##_tpi_type_num[(i)];\
TPI(t)\
{\
	t *_type;\
	_tpi_##t##_tpi_type_num *_type_num;\
	long object_off;\
}

/*
 * Declaration of typed persistentI of an object
 */
#define	TPI_DECLARE(t, i) _TPI_DECLARE(t, _TPI_TYPE_NUM(i))

/*
 * Declaration of typed persistenceI of a root object -- no need for this
 */
#define	TPI_DECLARE_ROOT(t) _TPI_DECLARE(t, POBJ_ROOT_TYPE_NUM)

/*
 * Type number of specified type
 */
#define	TPI_TYPE_NUM(t) (sizeof (_tpi_##t##_tpi_type_num))

/*
 * Type number of object read from typed OID
 */
#define	TPI_TYPE_NUM_OF(o) (sizeof (*(o)._type_num))

/*
 * NULL check
 */
#define	TPI_IS_NULL(o)((o).object_off == 0)

/*
 * Validates whether type number stored in typed OID is the same
 * as type number stored in object's metadata
 */
 // check where to incorp for TPI
#define	TPI_VALID(o) (TPI_TYPE_NUM_OF(o) == pmemobj_type_num((o)))

/*
 * Checks whether the object is of a given type
 */
#define	PI_INSTANCEOF(o, t) (TPI_TYPE_NUM(t) == pmemobj_type_num(o))


/*
 * Declaration of typed OID inside layout declaration
 */
 // check if any extra support if required for this
#define	POBJ_LAYOUT_TPI(name, t)\
TPI_DECLARE(t, (__COUNTER__ - _POBJ_LAYOUT_REF(name)));
/*
 * Declaration of typed OID of root inside layout declaration
 */
// check if any extra support if required for this
#define	TPI_POBJ_LAYOUT_ROOT(name, t)\
TPI_DECLARE_ROOT(t);
// returns the absoulte address of the root
#define POBJ_ROOT_OFF(pop, t) (*((pmemobj_root((pop), sizeof(t)))))
/*
#if RIV_MODE
	#define	POBJ_ALLOC_OFF(pop, offset, t, size, constr, arg) (\
	{	unsigned long long init = 0ULL; unsigned long long *riv = &init;\
		PMEMoid o = OID_NULL;\
		int outcome = 0;\
		outcome = pmemobj_alloc((pop), &o, riv, (size), TPI_TYPE_NUM(t), (constr), (arg));\
		(((intptr_t)writeoh(offset, pmemobj_direct(o)))|0x4000000000000000LL);})
#else
	#define	POBJ_ALLOC_OFF(pop, offset, t, size, constr, arg) (\
	{	PMEMoid o = OID_NULL;\
		pmemobj_alloc((pop), &o, (size), TPI_TYPE_NUM(t), (constr), (arg));\
		(((intptr_t)writeoh(offset, pmemobj_direct(o)))|0x4000000000000000LL);})
#endif
*/
#if RIV_MODE
	#define	POBJ_ALLOC_OFF(pop, t, size, constr, arg) (\
	{	unsigned long long init = 0ULL; unsigned long long *riv = &init;\
		PMEMoid o = OID_NULL;\
		int outcome = 0;\
		outcome = pmemobj_alloc((pop), &o, riv, (size), TPI_TYPE_NUM(t), (constr), (arg));\
		pmemobj_direct(o);\
;})
#else
	#define	POBJ_ALLOC_OFF(pop, t, size, constr, arg) (\
	{	PMEMoid o = OID_NULL;\
		pmemobj_alloc((pop), &o, (size), TPI_TYPE_NUM(t), (constr), (arg));\
		pmemobj_direct(o);})
#endif

// free the object

#if RIV_MODE
	#define POBJ_FREE_OFF(o)(\
		{PMEMoid oid = OID_NULL;\
		unsigned long long complete_address = (intptr_t)readoh(o);\
		unsigned long long a_mask = generateMask(l3);\
		unsigned long long offset = ((uintptr_t)complete_address)&a_mask;\
		unsigned long long base = ((uintptr_t)complete_address)&(~a_mask);\
		unsigned long long rid = getRID(base);\
		oid.pool_uuid_lo = rid;\
		oid.off = offset;\
		pmemobj_free(&oid);})
#elif BASED_MODE
	#define POBJ_FREE_OFF(o)(\
		{PMEMoid oid = OID_NULL;\
		int64_t complete_address = (intptr_t)readoh(o);\
		oid.pool_uuid_lo = based_id;\
		oid.off = (((intptr_t)complete_address)- (based_p));\
		pmemobj_free(&oid);})
#endif
//PRIiPTR
//printf("based_id %llx\n", (unsigned long long )based_id);\
// return the casted pointer back and also include the offset within the read
/*#define readoh(param)({\
int64_t address = 0LL;\
address = param ? ((((intptr_t)(&param))+ ((intptr_t)param))&0x3fffffffffffffffLL) : 0;\
(typeof((*param)) *) address;\
})*/
/*#define writeoh(paddr, oaddr)({\
fprintf(stderr, "paddr %lld\n", paddr);\
fprintf(stderr, "oaddr %lld\n", oaddr);\
fprintf(stderr, "offset calculated %lld\n", (intptr_t)oaddr - (intptr_t)paddr);\
fprintf(stderr, "offset %lld\n", (*paddr));
(*paddr) = ((intptr_t)oaddr - (intptr_t)paddr);})*/
/*#define writeoh(paddr, oaddr)({\
(*paddr) = (((intptr_t)oaddr - (intptr_t)paddr));\
(*paddr) = (intptr_t)(*paddr) ?  (((intptr_t)(*paddr))|0x4000000000000000LL): (*paddr);\
(*paddr) = oaddr ? (*paddr):0;})
*/

/*
#define readoh(param)({\
fprintf(stderr, "param is %llx\n", param);\
int64_t address = ((intptr_t)param)&0x3fffffffffffffffLL;\
fprintf(stderr, "address is %llx\n", address);\
int64_t mask = generateMask(l3);\
fprintf(stderr, "mask is %llx\n", mask);\
fprintf(stderr, "((1ULL << l3)&address) is %llx\n", ((1ULL << l3)&address));\
address = address ? (((1ULL << l3)&address) ? (((intptr_t)(&param))+(address|(~mask))) : (((intptr_t)(&param))+(address))) : 0;\
fprintf(stderr, "final address being returned is %llx\n", address);\
(typeof((*param)) *) address;\
})

#define writeoh(paddr, oaddr)({\
fprintf(stderr, "paddr %llx\n", paddr);\
fprintf(stderr, "oaddr %llx\n", oaddr);\
fprintf(stderr, "offset calculated %llx\n", (intptr_t)oaddr - (intptr_t)paddr);\
fprintf(stderr, "mask %llx\n", generateMask((l3+1)));\
int64_t address = (((intptr_t)oaddr - (intptr_t)paddr));\
int64_t mask = generateMask((l3+1));\
fprintf(stderr, "address&mask %llx\n", (address&mask));\
address = (address&mask);\
(*paddr) = address ?  ((address)|0x4000000000000000LL): 0;\
fprintf(stderr, "final value being written %llx\n", (*paddr));\
(*paddr) = oaddr ? (*paddr):0;})
*/

/*#define readoh(param)({\
int64_t address = ((intptr_t)param)&0x3fffffffffffffffLL;\
int64_t mask = generateMask(l3);\
address = address ? (((1ULL << l3)&address) ? (((intptr_t)(&param))+(address|(~mask))) : (((intptr_t)(&param))+(address))) : 0;\
(typeof((*param)) *) address;\
})

#define writeoh(paddr, oaddr)({\
int64_t address = (((intptr_t)oaddr - (intptr_t)paddr));\
int64_t mask = generateMask((l3+1));\
address = (address&mask);\
(*paddr) = address ?  ((address)|0x4000000000000000LL): 0;\
(*paddr) = oaddr ? (*paddr):0;})
*/

/*#define readoh(param)({\
fprintf(stderr, "param is %llx\n", param);\
fprintf(stderr, "bit is %llx\n", (((intptr_t)param&0x8000000000000000LL)));\
fprintf(stderr, "param value is %llx\n", (((intptr_t)param)&0x1fffffffffffffffLL));\
fprintf(stderr, "other param value is %llx\n", (((intptr_t)(&param))+(intptr_t)param));\
int64_t address  = param ? (((intptr_t)param&0x8000000000000000LL) ? (((intptr_t)(&param))+(intptr_t)param) : (((intptr_t)(&param))+(((intptr_t)param)&0x1fffffffffffffffLL))) : 0;\
fprintf(stderr, "address being generated %llx\n", address);\
(typeof((*param)) *) address;\
})

#define writeoh(paddr, oaddr)({\
int64_t address = (((intptr_t)oaddr - (intptr_t)paddr));\
fprintf(stderr, "address %llx\n", address);\
(*paddr) = oaddr ? ((address)|0x6000000000000000LL):0;})
*/

/*#define readoh(param)({\
int64_t address  = param ? (((intptr_t)param&0x8000000000000000LL) ? (((intptr_t)(&param))+(intptr_t)param) : (((intptr_t)(&param))+(((intptr_t)param)&0x1fffffffffffffffLL))) : 0;\
(typeof((*param)) *) address;\
})*/

/* 
fprintf(stderr, "param is %llx\n", param);\
fprintf(stderr, "address being generated %llx\n", (((intptr_t)(&param))+(((intptr_t)param)&0x00000000FFFFFFFFLL)));\
*/

/*#define readoh(param)({\
param ? (typeof((*param)) *)(((intptr_t)(&param))+(int)(((intptr_t)param)&0x00000000FFFFFFFFLL)) : 0;\
})*/

#define readoh(param)({\
param ? (typeof((*param)) *)(((intptr_t)(&param))+(int)(((intptr_t)param))) : 0;\
})

/*
#define readoh(param)({\
param ? (typeof((*param)) *)(((intptr_t)(&param))+(((intptr_t)param))) : 0;\
})
*/

#define writeoh(paddr, oaddr)({\
(*paddr) = oaddr ? (((((intptr_t)oaddr - (intptr_t)paddr)))|0x6000000000000000LL):0;})


/*#define writeoh(paddr, oaddr)({\
(*paddr) = oaddr ? ((((intptr_t)oaddr - (intptr_t)paddr))):0;})
*/

//fprintf(stderr, "address %llx\n", (((((intptr_t)oaddr - (intptr_t)paddr)))|0x6000000000000000LL));\

#ifdef __cplusplus
}
#endif
#endif	/* offholder.h */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"

#define _MULTIARRAYMODULE
#define NPY_NO_PREFIX
#include "numpy/arrayobject.h"
#include "numpy/arrayscalars.h"

#include "arrayobject.h"

#include "arrayctors.h"

/*
 * Change a sub-array field to the base descriptor
 *
 * and update the dimensions and strides
 * appropriately.  Dimensions and strides are added
 * to the end unless we have a FORTRAN array
 * and then they are added to the beginning
 *
 * Strides are only added if given (because data is given).
 */
static int
_update_descr_and_dimensions(PyArray_Descr **des, intp *newdims,
                             intp *newstrides, int oldnd, int isfortran)
{
    PyArray_Descr *old;
    int newnd;
    int numnew;
    intp *mydim;
    int i;
    int tuple;

    old = *des;
    *des = old->subarray->base;


    mydim = newdims + oldnd;
    tuple = PyTuple_Check(old->subarray->shape);
    if (tuple) {
        numnew = PyTuple_GET_SIZE(old->subarray->shape);
    }
    else {
        numnew = 1;
    }


    newnd = oldnd + numnew;
    if (newnd > MAX_DIMS) {
        goto finish;
    }
    if (isfortran) {
        memmove(newdims+numnew, newdims, oldnd*sizeof(intp));
        mydim = newdims;
    }
    if (tuple) {
        for (i = 0; i < numnew; i++) {
            mydim[i] = (intp) PyInt_AsLong(
                    PyTuple_GET_ITEM(old->subarray->shape, i));
        }
    }
    else {
        mydim[0] = (intp) PyInt_AsLong(old->subarray->shape);
    }

    if (newstrides) {
        intp tempsize;
        intp *mystrides;

        mystrides = newstrides + oldnd;
        if (isfortran) {
            memmove(newstrides+numnew, newstrides, oldnd*sizeof(intp));
            mystrides = newstrides;
        }
        /* Make new strides -- alwasy C-contiguous */
        tempsize = (*des)->elsize;
        for (i = numnew - 1; i >= 0; i--) {
            mystrides[i] = tempsize;
            tempsize *= mydim[i] ? mydim[i] : 1;
        }
    }

 finish:
    Py_INCREF(*des);
    Py_DECREF(old);
    return newnd;
}

/*
 * If s is not a list, return 0
 * Otherwise:
 *
 * run object_depth_and_dimension on all the elements
 * and make sure the returned shape and size is the
 * same for each element
 */
static int
object_depth_and_dimension(PyObject *s, int max, intp *dims)
{
    intp *newdims, *test_dims;
    int nd, test_nd;
    int i, islist, istuple;
    intp size;
    PyObject *obj;

    islist = PyList_Check(s);
    istuple = PyTuple_Check(s);
    if (!(islist || istuple)) {
        return 0;
    }

    size = PySequence_Size(s);
    if (size == 0) {
        return 0;
    }
    if (max < 1) {
        return 0;
    }
    if (max < 2) {
        dims[0] = size;
        return 1;
    }

    newdims = PyDimMem_NEW(2*(max - 1));
    test_dims = newdims + (max - 1);
    if (islist) {
        obj = PyList_GET_ITEM(s, 0);
    }
    else {
        obj = PyTuple_GET_ITEM(s, 0);
    }
    nd = object_depth_and_dimension(obj, max - 1, newdims);

    for (i = 1; i < size; i++) {
        if (islist) {
            obj = PyList_GET_ITEM(s, i);
        }
        else {
            obj = PyTuple_GET_ITEM(s, i);
        }
        test_nd = object_depth_and_dimension(obj, max-1, test_dims);

        if ((nd != test_nd) ||
            (!PyArray_CompareLists(newdims, test_dims, nd))) {
            nd = 0;
            break;
        }
    }

    for (i = 1; i <= nd; i++) {
        dims[i] = newdims[i-1];
    }
    dims[0] = size;
    PyDimMem_FREE(newdims);
    return nd + 1;
}

static void
_strided_byte_copy(char *dst, intp outstrides, char *src, intp instrides,
                   intp N, int elsize)
{
    intp i, j;
    char *tout = dst;
    char *tin = src;

#define _FAST_MOVE(_type_)                              \
    for(i=0; i<N; i++) {                               \
        ((_type_ *)tout)[0] = ((_type_ *)tin)[0];       \
        tin += instrides;                               \
        tout += outstrides;                             \
    }                                                   \
    return

    switch(elsize) {
    case 8:
        _FAST_MOVE(Int64);
    case 4:
        _FAST_MOVE(Int32);
    case 1:
        _FAST_MOVE(Int8);
    case 2:
        _FAST_MOVE(Int16);
    case 16:
        for (i = 0; i < N; i++) {
            ((Int64 *)tout)[0] = ((Int64 *)tin)[0];
            ((Int64 *)tout)[1] = ((Int64 *)tin)[1];
            tin += instrides;
            tout += outstrides;
        }
        return;
    default:
        for(i = 0; i < N; i++) {
            for(j=0; j<elsize; j++) {
                *tout++ = *tin++;
            }
            tin = tin + instrides - elsize;
            tout = tout + outstrides - elsize;
        }
    }
#undef _FAST_MOVE

}

static void
_unaligned_strided_byte_move(char *dst, intp outstrides, char *src,
                             intp instrides, intp N, int elsize)
{
    intp i;
    char *tout = dst;
    char *tin = src;


#define _MOVE_N_SIZE(size)                      \
    for(i=0; i<N; i++) {                       \
        memmove(tout, tin, size);               \
        tin += instrides;                       \
        tout += outstrides;                     \
    }                                           \
    return

    switch(elsize) {
    case 8:
        _MOVE_N_SIZE(8);
    case 4:
        _MOVE_N_SIZE(4);
    case 1:
        _MOVE_N_SIZE(1);
    case 2:
        _MOVE_N_SIZE(2);
    case 16:
        _MOVE_N_SIZE(16);
    default:
        _MOVE_N_SIZE(elsize);
    }
#undef _MOVE_N_SIZE

}

NPY_NO_EXPORT void
_unaligned_strided_byte_copy(char *dst, intp outstrides, char *src,
                             intp instrides, intp N, int elsize)
{
    intp i;
    char *tout = dst;
    char *tin = src;

#define _COPY_N_SIZE(size)                      \
    for(i=0; i<N; i++) {                       \
        memcpy(tout, tin, size);                \
        tin += instrides;                       \
        tout += outstrides;                     \
    }                                           \
    return

    switch(elsize) {
    case 8:
        _COPY_N_SIZE(8);
    case 4:
        _COPY_N_SIZE(4);
    case 1:
        _COPY_N_SIZE(1);
    case 2:
        _COPY_N_SIZE(2);
    case 16:
        _COPY_N_SIZE(16);
    default:
        _COPY_N_SIZE(elsize);
    }
#undef _COPY_N_SIZE

}

NPY_NO_EXPORT void
_strided_byte_swap(void *p, intp stride, intp n, int size)
{
    char *a, *b, c = 0;
    int j, m;

    switch(size) {
    case 1: /* no byteswap necessary */
        break;
    case 4:
        for (a = (char*)p; n > 0; n--, a += stride - 1) {
            b = a + 3;
            c = *a; *a++ = *b; *b-- = c;
            c = *a; *a = *b; *b   = c;
        }
        break;
    case 8:
        for (a = (char*)p; n > 0; n--, a += stride - 3) {
            b = a + 7;
            c = *a; *a++ = *b; *b-- = c;
            c = *a; *a++ = *b; *b-- = c;
            c = *a; *a++ = *b; *b-- = c;
            c = *a; *a = *b; *b   = c;
        }
        break;
    case 2:
        for (a = (char*)p; n > 0; n--, a += stride) {
            b = a + 1;
            c = *a; *a = *b; *b = c;
        }
        break;
    default:
        m = size/2;
        for (a = (char *)p; n > 0; n--, a += stride - m) {
            b = a + (size - 1);
            for (j = 0; j < m; j++) {
                c=*a; *a++ = *b; *b-- = c;
            }
        }
        break;
    }
}

NPY_NO_EXPORT void
byte_swap_vector(void *p, intp n, int size)
{
    _strided_byte_swap(p, (intp) size, n, size);
    return;
}

/* If numitems > 1, then dst must be contiguous */
NPY_NO_EXPORT void
copy_and_swap(void *dst, void *src, int itemsize, intp numitems,
              intp srcstrides, int swap)
{
    intp i;
    char *s1 = (char *)src;
    char *d1 = (char *)dst;


    if ((numitems == 1) || (itemsize == srcstrides)) {
        memcpy(d1, s1, itemsize*numitems);
    }
    else {
        for (i = 0; i < numitems; i++) {
            memcpy(d1, s1, itemsize);
            d1 += itemsize;
            s1 += srcstrides;
        }
    }

    if (swap) {
        byte_swap_vector(d1, numitems, itemsize);
    }
}

static int
_copy_from0d(PyArrayObject *dest, PyArrayObject *src, int usecopy, int swap)
{
    char *aligned = NULL;
    char *sptr;
    int numcopies, nbytes;
    void (*myfunc)(char *, intp, char *, intp, intp, int);
    int retval = -1;
    NPY_BEGIN_THREADS_DEF;

    numcopies = PyArray_SIZE(dest);
    if (numcopies < 1) {
        return 0;
    }
    nbytes = PyArray_ITEMSIZE(src);

    if (!PyArray_ISALIGNED(src)) {
        aligned = malloc((size_t)nbytes);
        if (aligned == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        memcpy(aligned, src->data, (size_t) nbytes);
        usecopy = 1;
        sptr = aligned;
    }
    else {
        sptr = src->data;
    }
    if (PyArray_SAFEALIGNEDCOPY(dest)) {
        myfunc = _strided_byte_copy;
    }
    else if (usecopy) {
        myfunc = _unaligned_strided_byte_copy;
    }
    else {
        myfunc = _unaligned_strided_byte_move;
    }

    if ((dest->nd < 2) || PyArray_ISONESEGMENT(dest)) {
        char *dptr;
        intp dstride;

        dptr = dest->data;
        if (dest->nd == 1) {
            dstride = dest->strides[0];
        }
        else {
            dstride = nbytes;
        }

        /* Refcount note: src and dest may have different sizes */
        PyArray_INCREF(src);
        PyArray_XDECREF(dest);
        NPY_BEGIN_THREADS;
        myfunc(dptr, dstride, sptr, 0, numcopies, (int) nbytes);
        if (swap) {
            _strided_byte_swap(dptr, dstride, numcopies, (int) nbytes);
        }
        NPY_END_THREADS;
        PyArray_INCREF(dest);
        PyArray_XDECREF(src);
    }
    else {
        PyArrayIterObject *dit;
        int axis = -1;

        dit = (PyArrayIterObject *)
            PyArray_IterAllButAxis((PyObject *)dest, &axis);
        if (dit == NULL) {
            goto finish;
        }
        /* Refcount note: src and dest may have different sizes */
        PyArray_INCREF(src);
        PyArray_XDECREF(dest);
        NPY_BEGIN_THREADS;
        while(dit->index < dit->size) {
            myfunc(dit->dataptr, PyArray_STRIDE(dest, axis), sptr, 0,
                    PyArray_DIM(dest, axis), nbytes);
            if (swap) {
                _strided_byte_swap(dit->dataptr, PyArray_STRIDE(dest, axis),
                        PyArray_DIM(dest, axis), nbytes);
            }
            PyArray_ITER_NEXT(dit);
        }
        NPY_END_THREADS;
        PyArray_INCREF(dest);
        PyArray_XDECREF(src);
        Py_DECREF(dit);
    }
    retval = 0;

finish:
    if (aligned != NULL) {
        free(aligned);
    }
    return retval;
}

/*
 * Special-case of PyArray_CopyInto when dst is 1-d
 * and contiguous (and aligned).
 * PyArray_CopyInto requires broadcastable arrays while
 * this one is a flattening operation...
 */
NPY_NO_EXPORT int
_flat_copyinto(PyObject *dst, PyObject *src, NPY_ORDER order)
{
    PyArrayIterObject *it;
    PyObject *orig_src;
    void (*myfunc)(char *, intp, char *, intp, intp, int);
    char *dptr;
    int axis;
    int elsize;
    intp nbytes;
    NPY_BEGIN_THREADS_DEF;


    orig_src = src;
    if (PyArray_NDIM(src) == 0) {
        /* Refcount note: src and dst have the same size */
        PyArray_INCREF((PyArrayObject *)src);
        PyArray_XDECREF((PyArrayObject *)dst);
        NPY_BEGIN_THREADS;
        memcpy(PyArray_BYTES(dst), PyArray_BYTES(src),
                PyArray_ITEMSIZE(src));
        NPY_END_THREADS;
        return 0;
    }

    axis = PyArray_NDIM(src)-1;

    if (order == PyArray_FORTRANORDER) {
        if (PyArray_NDIM(src) <= 2) {
            axis = 0;
        }
        /* fall back to a more general method */
        else {
            src = PyArray_Transpose((PyArrayObject *)orig_src, NULL);
        }
    }

    it = (PyArrayIterObject *)PyArray_IterAllButAxis(src, &axis);
    if (it == NULL) {
        if (src != orig_src) {
            Py_DECREF(src);
        }
        return -1;
    }

    if (PyArray_SAFEALIGNEDCOPY(src)) {
        myfunc = _strided_byte_copy;
    }
    else {
        myfunc = _unaligned_strided_byte_copy;
    }

    dptr = PyArray_BYTES(dst);
    elsize = PyArray_ITEMSIZE(dst);
    nbytes = elsize * PyArray_DIM(src, axis);

    /* Refcount note: src and dst have the same size */
    PyArray_INCREF((PyArrayObject *)src);
    PyArray_XDECREF((PyArrayObject *)dst);
    NPY_BEGIN_THREADS;
    while(it->index < it->size) {
        myfunc(dptr, elsize, it->dataptr, PyArray_STRIDE(src,axis),
                PyArray_DIM(src,axis), elsize);
        dptr += nbytes;
        PyArray_ITER_NEXT(it);
    }
    NPY_END_THREADS;

    if (src != orig_src) {
        Py_DECREF(src);
    }
    Py_DECREF(it);
    return 0;
}


static int
_copy_from_same_shape(PyArrayObject *dest, PyArrayObject *src,
                      void (*myfunc)(char *, intp, char *, intp, intp, int),
                      int swap)
{
    int maxaxis = -1, elsize;
    intp maxdim;
    PyArrayIterObject *dit, *sit;
    NPY_BEGIN_THREADS_DEF;

    dit = (PyArrayIterObject *)
        PyArray_IterAllButAxis((PyObject *)dest, &maxaxis);
    sit = (PyArrayIterObject *)
        PyArray_IterAllButAxis((PyObject *)src, &maxaxis);

    maxdim = dest->dimensions[maxaxis];

    if ((dit == NULL) || (sit == NULL)) {
        Py_XDECREF(dit);
        Py_XDECREF(sit);
        return -1;
    }
    elsize = PyArray_ITEMSIZE(dest);

    /* Refcount note: src and dst have the same size */
    PyArray_INCREF(src);
    PyArray_XDECREF(dest);

    NPY_BEGIN_THREADS;
    while(dit->index < dit->size) {
        /* strided copy of elsize bytes */
        myfunc(dit->dataptr, dest->strides[maxaxis],
                sit->dataptr, src->strides[maxaxis],
                maxdim, elsize);
        if (swap) {
            _strided_byte_swap(dit->dataptr,
                    dest->strides[maxaxis],
                    dest->dimensions[maxaxis],
                    elsize);
        }
        PyArray_ITER_NEXT(dit);
        PyArray_ITER_NEXT(sit);
    }
    NPY_END_THREADS;

    Py_DECREF(sit);
    Py_DECREF(dit);
    return 0;
}

static int
_broadcast_copy(PyArrayObject *dest, PyArrayObject *src,
                void (*myfunc)(char *, intp, char *, intp, intp, int),
                int swap)
{
    int elsize;
    PyArrayMultiIterObject *multi;
    int maxaxis; intp maxdim;
    NPY_BEGIN_THREADS_DEF;

    elsize = PyArray_ITEMSIZE(dest);
    multi = (PyArrayMultiIterObject *)PyArray_MultiIterNew(2, dest, src);
    if (multi == NULL) {
        return -1;
    }

    if (multi->size != PyArray_SIZE(dest)) {
        PyErr_SetString(PyExc_ValueError,
                "array dimensions are not "\
                "compatible for copy");
        Py_DECREF(multi);
        return -1;
    }

    maxaxis = PyArray_RemoveSmallest(multi);
    if (maxaxis < 0) {
        /*
         * copy 1 0-d array to another
         * Refcount note: src and dst have the same size
         */
        PyArray_INCREF(src);
        PyArray_XDECREF(dest);
        memcpy(dest->data, src->data, elsize);
        if (swap) {
            byte_swap_vector(dest->data, 1, elsize);
        }
        return 0;
    }
    maxdim = multi->dimensions[maxaxis];

    /*
     * Increment the source and decrement the destination
     * reference counts
     *
     * Refcount note: src and dest may have different sizes
     */
    PyArray_INCREF(src);
    PyArray_XDECREF(dest);

    NPY_BEGIN_THREADS;
    while(multi->index < multi->size) {
        myfunc(multi->iters[0]->dataptr,
                multi->iters[0]->strides[maxaxis],
                multi->iters[1]->dataptr,
                multi->iters[1]->strides[maxaxis],
                maxdim, elsize);
        if (swap) {
            _strided_byte_swap(multi->iters[0]->dataptr,
                    multi->iters[0]->strides[maxaxis],
                    maxdim, elsize);
        }
        PyArray_MultiIter_NEXT(multi);
    }
    NPY_END_THREADS;

    PyArray_INCREF(dest);
    PyArray_XDECREF(src);

    Py_DECREF(multi);
    return 0;
}

/* If destination is not the right type, then src
   will be cast to destination -- this requires
   src and dest to have the same shape
*/

/* Requires arrays to have broadcastable shapes

   The arrays are assumed to have the same number of elements
   They can be different sizes and have different types however.
*/

static int
_array_copy_into(PyArrayObject *dest, PyArrayObject *src, int usecopy)
{
    int swap;
    void (*myfunc)(char *, intp, char *, intp, intp, int);
    int simple;
    int same;
    NPY_BEGIN_THREADS_DEF;


    if (!PyArray_EquivArrTypes(dest, src)) {
        return PyArray_CastTo(dest, src);
    }
    if (!PyArray_ISWRITEABLE(dest)) {
        PyErr_SetString(PyExc_RuntimeError,
                "cannot write to array");
        return -1;
    }
    same = PyArray_SAMESHAPE(dest, src);
    simple = same && ((PyArray_ISCARRAY_RO(src) && PyArray_ISCARRAY(dest)) ||
            (PyArray_ISFARRAY_RO(src) && PyArray_ISFARRAY(dest)));

    if (simple) {
        /* Refcount note: src and dest have the same size */
        PyArray_INCREF(src);
        PyArray_XDECREF(dest);
        NPY_BEGIN_THREADS;
        if (usecopy) {
            memcpy(dest->data, src->data, PyArray_NBYTES(dest));
        }
        else {
            memmove(dest->data, src->data, PyArray_NBYTES(dest));
        }
        NPY_END_THREADS;
        return 0;
    }

    swap = PyArray_ISNOTSWAPPED(dest) != PyArray_ISNOTSWAPPED(src);

    if (src->nd == 0) {
        return _copy_from0d(dest, src, usecopy, swap);
    }

    if (PyArray_SAFEALIGNEDCOPY(dest) && PyArray_SAFEALIGNEDCOPY(src)) {
        myfunc = _strided_byte_copy;
    }
    else if (usecopy) {
        myfunc = _unaligned_strided_byte_copy;
    }
    else {
        myfunc = _unaligned_strided_byte_move;
    }
    /*
     * Could combine these because _broadcasted_copy would work as well.
     * But, same-shape copying is so common we want to speed it up.
     */
    if (same) {
        return _copy_from_same_shape(dest, src, myfunc, swap);
    }
    else {
        return _broadcast_copy(dest, src, myfunc, swap);
    }
}

/*NUMPY_API
 * Move the memory of one array into another.
 */
NPY_NO_EXPORT int
PyArray_MoveInto(PyArrayObject *dest, PyArrayObject *src)
{
    return _array_copy_into(dest, src, 0);
}



/* adapted from Numarray */
static int
setArrayFromSequence(PyArrayObject *a, PyObject *s, int dim, intp offset)
{
    Py_ssize_t i, slen;
    int res = 0;

    /*
     * This code is to ensure that the sequence access below will
     * return a lower-dimensional sequence.
     */
    if (PyArray_Check(s) && !(PyArray_CheckExact(s))) {
      /*
       * FIXME:  This could probably copy the entire subarray at once here using
       * a faster algorithm.  Right now, just make sure a base-class array is
       * used so that the dimensionality reduction assumption is correct.
       */
	s = PyArray_EnsureArray(s);
    }

    if (dim > a->nd) {
        PyErr_Format(PyExc_ValueError,
                     "setArrayFromSequence: sequence/array dimensions mismatch.");
        return -1;
    }

    slen = PySequence_Length(s);
    if (slen != a->dimensions[dim]) {
        PyErr_Format(PyExc_ValueError,
                     "setArrayFromSequence: sequence/array shape mismatch.");
        return -1;
    }

    for (i = 0; i < slen; i++) {
        PyObject *o = PySequence_GetItem(s, i);
        if ((a->nd - dim) > 1) {
            res = setArrayFromSequence(a, o, dim+1, offset);
        }
        else {
            res = a->descr->f->setitem(o, (a->data + offset), a);
        }
        Py_DECREF(o);
        if (res < 0) {
            return res;
        }
        offset += a->strides[dim];
    }
    return 0;
}

static int
Assign_Array(PyArrayObject *self, PyObject *v)
{
    if (!PySequence_Check(v)) {
        PyErr_SetString(PyExc_ValueError,
                        "assignment from non-sequence");
        return -1;
    }
    if (self->nd == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "assignment to 0-d array");
        return -1;
    }
    return setArrayFromSequence(self, v, 0, 0);
}

/*
 * "Array Scalars don't call this code"
 * steals reference to typecode -- no NULL
 */
static PyObject *
Array_FromPyScalar(PyObject *op, PyArray_Descr *typecode)
{
    PyArrayObject *ret;
    int itemsize;
    int type;

    itemsize = typecode->elsize;
    type = typecode->type_num;

    if (itemsize == 0 && PyTypeNum_ISEXTENDED(type)) {
        itemsize = PyObject_Length(op);
        if (type == PyArray_UNICODE) {
            itemsize *= 4;
        }
        if (itemsize != typecode->elsize) {
            PyArray_DESCR_REPLACE(typecode);
            typecode->elsize = itemsize;
        }
    }

    ret = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type, typecode,
                                                0, NULL,
                                                NULL, NULL, 0, NULL);
    if (ret == NULL) {
        return NULL;
    }
    if (ret->nd > 0) {
        PyErr_SetString(PyExc_ValueError,
                        "shape-mismatch on array construction");
        Py_DECREF(ret);
        return NULL;
    }

    ret->descr->f->setitem(op, ret->data, ret);
    if (PyErr_Occurred()) {
        Py_DECREF(ret);
        return NULL;
    }
    else {
        return (PyObject *)ret;
    }
}


static PyObject *
ObjectArray_FromNestedList(PyObject *s, PyArray_Descr *typecode, int fortran)
{
    int nd;
    intp d[MAX_DIMS];
    PyArrayObject *r;

    /* Get the depth and the number of dimensions */
    nd = object_depth_and_dimension(s, MAX_DIMS, d);
    if (nd < 0) {
        return NULL;
    }
    if (nd == 0) {
        return Array_FromPyScalar(s, typecode);
    }
    r = (PyArrayObject*)PyArray_NewFromDescr(&PyArray_Type, typecode,
                                             nd, d,
                                             NULL, NULL,
                                             fortran, NULL);
    if (!r) {
        return NULL;
    }
    if(Assign_Array(r,s) == -1) {
        Py_DECREF(r);
        return NULL;
    }
    return (PyObject*)r;
}

/*
 * The rest of this code is to build the right kind of array
 * from a python object.
 */

static int
discover_depth(PyObject *s, int max, int stop_at_string, int stop_at_tuple)
{
    int d = 0;
    PyObject *e;

    if(max < 1) {
        return -1;
    }
    if(!PySequence_Check(s) || PyInstance_Check(s) ||
            PySequence_Length(s) < 0) {
        PyErr_Clear();
        return 0;
    }
    if (PyArray_Check(s)) {
        return PyArray_NDIM(s);
    }
    if (PyArray_IsScalar(s, Generic)) {
        return 0;
    }
    if (PyString_Check(s) || PyBuffer_Check(s) || PyUnicode_Check(s)) {
        return stop_at_string ? 0:1;
    }
    if (stop_at_tuple && PyTuple_Check(s)) {
        return 0;
    }
    if ((e = PyObject_GetAttrString(s, "__array_struct__")) != NULL) {
        d = -1;
        if (PyCObject_Check(e)) {
            PyArrayInterface *inter;
            inter = (PyArrayInterface *)PyCObject_AsVoidPtr(e);
            if (inter->two == 2) {
                d = inter->nd;
            }
        }
        Py_DECREF(e);
        if (d > -1) {
            return d;
        }
    }
    else {
        PyErr_Clear();
    }
    if ((e=PyObject_GetAttrString(s, "__array_interface__")) != NULL) {
        d = -1;
        if (PyDict_Check(e)) {
            PyObject *new;
            new = PyDict_GetItemString(e, "shape");
            if (new && PyTuple_Check(new)) {
                d = PyTuple_GET_SIZE(new);
            }
        }
        Py_DECREF(e);
        if (d>-1) {
            return d;
        }
    }
    else PyErr_Clear();

    if (PySequence_Length(s) == 0) {
        return 1;
    }
    if ((e=PySequence_GetItem(s,0)) == NULL) {
        return -1;
    }
    if (e != s) {
        d = discover_depth(e, max-1, stop_at_string, stop_at_tuple);
        if (d >= 0) {
            d++;
        }
    }
    Py_DECREF(e);
    return d;
}

static int
discover_itemsize(PyObject *s, int nd, int *itemsize)
{
    int n, r, i;
    PyObject *e;

    if (PyArray_Check(s)) {
	*itemsize = MAX(*itemsize, PyArray_ITEMSIZE(s));
	return 0;
    }

    n = PyObject_Length(s);
    if ((nd == 0) || PyString_Check(s) ||
            PyUnicode_Check(s) || PyBuffer_Check(s)) {
        *itemsize = MAX(*itemsize, n);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if ((e = PySequence_GetItem(s,i))==NULL) {
            return -1;
        }
        r = discover_itemsize(e,nd-1,itemsize);
        Py_DECREF(e);
        if (r == -1) {
            return -1;
        }
    }
    return 0;
}

/*
 * Take an arbitrary object known to represent
 * an array of ndim nd, and determine the size in each dimension
 */
static int
discover_dimensions(PyObject *s, int nd, intp *d, int check_it)
{
    PyObject *e;
    int r, n, i, n_lower;


    if (PyArray_Check(s)) {
	for (i=0; i<nd; i++) {
	    d[i] = PyArray_DIM(s,i);
	}
	return 0;
    }
    n = PyObject_Length(s);
    *d = n;
    if (*d < 0) {
        return -1;
    }
    if (nd <= 1) {
        return 0;
    }
    n_lower = 0;
    for(i = 0; i < n; i++) {
        if ((e = PySequence_GetItem(s,i)) == NULL) {
            return -1;
        }
        r = discover_dimensions(e, nd - 1, d + 1, check_it);
        Py_DECREF(e);

        if (r == -1) {
            return -1;
        }
        if (check_it && n_lower != 0 && n_lower != d[1]) {
            PyErr_SetString(PyExc_ValueError,
                            "inconsistent shape in sequence");
            return -1;
        }
        if (d[1] > n_lower) {
            n_lower = d[1];
        }
    }
    d[1] = n_lower;

    return 0;
}

/*
 * isobject means that we are constructing an
 * object array on-purpose with a nested list.
 * Only a list is interpreted as a sequence with these rules
 * steals reference to typecode
 */
static PyObject *
Array_FromSequence(PyObject *s, PyArray_Descr *typecode, int fortran,
                   int min_depth, int max_depth)
{
    PyArrayObject *r;
    int nd;
    int err;
    intp d[MAX_DIMS];
    int stop_at_string;
    int stop_at_tuple;
    int check_it;
    int type = typecode->type_num;
    int itemsize = typecode->elsize;

    check_it = (typecode->type != PyArray_CHARLTR);
    stop_at_string = (type != PyArray_STRING) ||
                     (typecode->type == PyArray_STRINGLTR);
    stop_at_tuple = (type == PyArray_VOID && (typecode->names
                                              || typecode->subarray));

    nd = discover_depth(s, MAX_DIMS + 1, stop_at_string, stop_at_tuple);
    if (nd == 0) {
        return Array_FromPyScalar(s, typecode);
    }
    else if (nd < 0) {
        PyErr_SetString(PyExc_ValueError,
                "invalid input sequence");
        goto fail;
    }
    if (max_depth && PyTypeNum_ISOBJECT(type) && (nd > max_depth)) {
        nd = max_depth;
    }
    if ((max_depth && nd > max_depth) || (min_depth && nd < min_depth)) {
        PyErr_SetString(PyExc_ValueError,
                "invalid number of dimensions");
        goto fail;
    }

    err = discover_dimensions(s, nd, d, check_it);
    if (err == -1) {
        goto fail;
    }
    if (typecode->type == PyArray_CHARLTR && nd > 0 && d[nd - 1] == 1) {
        nd = nd - 1;
    }

    if (itemsize == 0 && PyTypeNum_ISEXTENDED(type)) {
        err = discover_itemsize(s, nd, &itemsize);
        if (err == -1) {
            goto fail;
        }
        if (type == PyArray_UNICODE) {
            itemsize *= 4;
        }
    }
    if (itemsize != typecode->elsize) {
        PyArray_DESCR_REPLACE(typecode);
        typecode->elsize = itemsize;
    }

    r = (PyArrayObject*)PyArray_NewFromDescr(&PyArray_Type, typecode,
                                             nd, d,
                                             NULL, NULL,
                                             fortran, NULL);
    if (!r) {
        return NULL;
    }

    err = Assign_Array(r,s);
    if (err == -1) {
        Py_DECREF(r);
        return NULL;
    }
    return (PyObject*)r;

 fail:
    Py_DECREF(typecode);
    return NULL;
}



/*NUMPY_API
 * Generic new array creation routine.
 *
 * steals a reference to descr (even on failure)
 */
NPY_NO_EXPORT PyObject *
PyArray_NewFromDescr(PyTypeObject *subtype, PyArray_Descr *descr, int nd,
                     intp *dims, intp *strides, void *data,
                     int flags, PyObject *obj)
{
    PyArrayObject *self;
    int i;
    size_t sd;
    intp largest;
    intp size;

    if (descr->subarray) {
        PyObject *ret;
        intp newdims[2*MAX_DIMS];
        intp *newstrides = NULL;
        int isfortran = 0;
        isfortran = (data && (flags & FORTRAN) && !(flags & CONTIGUOUS)) ||
            (!data && flags);
        memcpy(newdims, dims, nd*sizeof(intp));
        if (strides) {
            newstrides = newdims + MAX_DIMS;
            memcpy(newstrides, strides, nd*sizeof(intp));
        }
        nd =_update_descr_and_dimensions(&descr, newdims,
                                         newstrides, nd, isfortran);
        ret = PyArray_NewFromDescr(subtype, descr, nd, newdims,
                                   newstrides,
                                   data, flags, obj);
        return ret;
    }
    if (nd < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "number of dimensions must be >=0");
        Py_DECREF(descr);
        return NULL;
    }
    if (nd > MAX_DIMS) {
        PyErr_Format(PyExc_ValueError,
                     "maximum number of dimensions is %d", MAX_DIMS);
        Py_DECREF(descr);
        return NULL;
    }

    /* Check dimensions */
    size = 1;
    sd = (size_t) descr->elsize;
    if (sd == 0) {
        if (!PyDataType_ISSTRING(descr)) {
            PyErr_SetString(PyExc_ValueError, "Empty data-type");
            Py_DECREF(descr);
            return NULL;
        }
        PyArray_DESCR_REPLACE(descr);
        if (descr->type_num == NPY_STRING) {
            descr->elsize = 1;
        }
        else {
            descr->elsize = sizeof(PyArray_UCS4);
        }
        sd = descr->elsize;
    }

    largest = NPY_MAX_INTP / sd;
    for (i = 0; i < nd; i++) {
        intp dim = dims[i];

        if (dim == 0) {
            /*
             * Compare to PyArray_OverflowMultiplyList that
             * returns 0 in this case.
             */
            continue;
        }
        if (dim < 0) {
            PyErr_SetString(PyExc_ValueError,
                            "negative dimensions "  \
                            "are not allowed");
            Py_DECREF(descr);
            return NULL;
        }
        if (dim > largest) {
            PyErr_SetString(PyExc_ValueError,
                            "array is too big.");
            Py_DECREF(descr);
            return NULL;
        }
        size *= dim;
        largest /= dim;
    }

    self = (PyArrayObject *) subtype->tp_alloc(subtype, 0);
    if (self == NULL) {
        Py_DECREF(descr);
        return NULL;
    }
    self->nd = nd;
    self->dimensions = NULL;
    self->data = NULL;
    if (data == NULL) {
        self->flags = DEFAULT;
        if (flags) {
            self->flags |= FORTRAN;
            if (nd > 1) {
                self->flags &= ~CONTIGUOUS;
            }
            flags = FORTRAN;
        }
    }
    else {
        self->flags = (flags & ~UPDATEIFCOPY);
    }
    self->descr = descr;
    self->base = (PyObject *)NULL;
    self->weakreflist = (PyObject *)NULL;

    if (nd > 0) {
        self->dimensions = PyDimMem_NEW(2*nd);
        if (self->dimensions == NULL) {
            PyErr_NoMemory();
            goto fail;
        }
        self->strides = self->dimensions + nd;
        memcpy(self->dimensions, dims, sizeof(intp)*nd);
        if (strides == NULL) { /* fill it in */
            sd = _array_fill_strides(self->strides, dims, nd, sd,
                                     flags, &(self->flags));
        }
        else {
            /*
             * we allow strides even when we create
             * the memory, but be careful with this...
             */
            memcpy(self->strides, strides, sizeof(intp)*nd);
            sd *= size;
        }
    }
    else {
        self->dimensions = self->strides = NULL;
    }

    if (data == NULL) {
        /*
         * Allocate something even for zero-space arrays
         * e.g. shape=(0,) -- otherwise buffer exposure
         * (a.data) doesn't work as it should.
         */

        if (sd == 0) {
            sd = descr->elsize;
        }
        if ((data = PyDataMem_NEW(sd)) == NULL) {
            PyErr_NoMemory();
            goto fail;
        }
        self->flags |= OWNDATA;

        /*
         * It is bad to have unitialized OBJECT pointers
         * which could also be sub-fields of a VOID array
         */
        if (PyDataType_FLAGCHK(descr, NPY_NEEDS_INIT)) {
            memset(data, 0, sd);
        }
    }
    else {
        /*
         * If data is passed in, this object won't own it by default.
         * Caller must arrange for this to be reset if truly desired
         */
        self->flags &= ~OWNDATA;
    }
    self->data = data;

    /*
     * call the __array_finalize__
     * method if a subtype.
     * If obj is NULL, then call method with Py_None
     */
    if ((subtype != &PyArray_Type)) {
        PyObject *res, *func, *args;
        static PyObject *str = NULL;

        if (str == NULL) {
            str = PyString_InternFromString("__array_finalize__");
        }
        func = PyObject_GetAttr((PyObject *)self, str);
        if (func && func != Py_None) {
            if (strides != NULL) {
                /*
                 * did not allocate own data or funny strides
                 * update flags before finalize function
                 */
                PyArray_UpdateFlags(self, UPDATE_ALL);
            }
            if PyCObject_Check(func) {
                /* A C-function is stored here */
                PyArray_FinalizeFunc *cfunc;
                cfunc = PyCObject_AsVoidPtr(func);
                Py_DECREF(func);
                if (cfunc(self, obj) < 0) {
                    goto fail;
                }
            }
            else {
                args = PyTuple_New(1);
                if (obj == NULL) {
                    obj=Py_None;
                }
                Py_INCREF(obj);
                PyTuple_SET_ITEM(args, 0, obj);
                res = PyObject_Call(func, args, NULL);
                Py_DECREF(args);
                Py_DECREF(func);
                if (res == NULL) {
                    goto fail;
                }
                else {
                    Py_DECREF(res);
                }
            }
        }
        else Py_XDECREF(func);
    }
    return (PyObject *)self;

 fail:
    Py_DECREF(self);
    return NULL;
}

/*NUMPY_API
 * Generic new array creation routine.
 */
NPY_NO_EXPORT PyObject *
PyArray_New(PyTypeObject *subtype, int nd, intp *dims, int type_num,
            intp *strides, void *data, int itemsize, int flags,
            PyObject *obj)
{
    PyArray_Descr *descr;
    PyObject *new;

    descr = PyArray_DescrFromType(type_num);
    if (descr == NULL) {
        return NULL;
    }
    if (descr->elsize == 0) {
        if (itemsize < 1) {
            PyErr_SetString(PyExc_ValueError,
                            "data type must provide an itemsize");
            Py_DECREF(descr);
            return NULL;
        }
        PyArray_DESCR_REPLACE(descr);
        descr->elsize = itemsize;
    }
    new = PyArray_NewFromDescr(subtype, descr, nd, dims, strides,
                               data, flags, obj);
    return new;
}

/*NUMPY_API
 * Does not check for ENSURECOPY and NOTSWAPPED in flags
 * Steals a reference to newtype --- which can be NULL
 */
NPY_NO_EXPORT PyObject *
PyArray_FromAny(PyObject *op, PyArray_Descr *newtype, int min_depth,
                int max_depth, int flags, PyObject *context)
{
    /*
     * This is the main code to make a NumPy array from a Python
     * Object.  It is called from lot's of different places which
     * is why there are so many checks.  The comments try to
     * explain some of the checks.
     */
    PyObject *r = NULL;
    int seq = FALSE;

    /*
     * Is input object already an array?
     * This is where the flags are used
     */
    if (PyArray_Check(op)) {
        r = PyArray_FromArray((PyArrayObject *)op, newtype, flags);
    }
    else if (PyArray_IsScalar(op, Generic)) {
        if (flags & UPDATEIFCOPY) {
            goto err;
        }
        r = PyArray_FromScalar(op, newtype);
    }
    else if (newtype == NULL &&
               (newtype = _array_find_python_scalar_type(op))) {
        if (flags & UPDATEIFCOPY) {
            goto err;
        }
        r = Array_FromPyScalar(op, newtype);
    }
    else if (PyArray_HasArrayInterfaceType(op, newtype, context, r)) {
        PyObject *new;
        if (r == NULL) {
            Py_XDECREF(newtype);
            return NULL;
        }
        if (newtype != NULL || flags != 0) {
            new = PyArray_FromArray((PyArrayObject *)r, newtype, flags);
            Py_DECREF(r);
            r = new;
        }
    }
    else {
        int isobject = 0;

        if (flags & UPDATEIFCOPY) {
            goto err;
        }
        if (newtype == NULL) {
            newtype = _array_find_type(op, NULL, MAX_DIMS);
        }
        else if (newtype->type_num == PyArray_OBJECT) {
            isobject = 1;
        }
        if (PySequence_Check(op)) {
            PyObject *thiserr = NULL;

            /* necessary but not sufficient */
            Py_INCREF(newtype);
            r = Array_FromSequence(op, newtype, flags & FORTRAN,
                                   min_depth, max_depth);
            if (r == NULL && (thiserr=PyErr_Occurred())) {
                if (PyErr_GivenExceptionMatches(thiserr,
                                                PyExc_MemoryError)) {
                    return NULL;
                }
                /*
                 * If object was explicitly requested,
                 * then try nested list object array creation
                 */
                PyErr_Clear();
                if (isobject) {
                    Py_INCREF(newtype);
                    r = ObjectArray_FromNestedList
                        (op, newtype, flags & FORTRAN);
                    seq = TRUE;
                    Py_DECREF(newtype);
                }
            }
            else {
                seq = TRUE;
                Py_DECREF(newtype);
            }
        }
        if (!seq) {
            r = Array_FromPyScalar(op, newtype);
        }
    }

    /* If we didn't succeed return NULL */
    if (r == NULL) {
        return NULL;
    }

    /* Be sure we succeed here */
    if(!PyArray_Check(r)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "internal error: PyArray_FromAny "\
                        "not producing an array");
        Py_DECREF(r);
        return NULL;
    }

    if (min_depth != 0 && ((PyArrayObject *)r)->nd < min_depth) {
        PyErr_SetString(PyExc_ValueError,
                        "object of too small depth for desired array");
        Py_DECREF(r);
        return NULL;
    }
    if (max_depth != 0 && ((PyArrayObject *)r)->nd > max_depth) {
        PyErr_SetString(PyExc_ValueError,
                        "object too deep for desired array");
        Py_DECREF(r);
        return NULL;
    }
    return r;

 err:
    Py_XDECREF(newtype);
    PyErr_SetString(PyExc_TypeError,
                    "UPDATEIFCOPY used for non-array input.");
    return NULL;
}

/*
 * flags is any of
 * CONTIGUOUS,
 * FORTRAN,
 * ALIGNED,
 * WRITEABLE,
 * NOTSWAPPED,
 * ENSURECOPY,
 * UPDATEIFCOPY,
 * FORCECAST,
 * ENSUREARRAY,
 * ELEMENTSTRIDES
 *
 * or'd (|) together
 *
 * Any of these flags present means that the returned array should
 * guarantee that aspect of the array.  Otherwise the returned array
 * won't guarantee it -- it will depend on the object as to whether or
 * not it has such features.
 *
 * Note that ENSURECOPY is enough
 * to guarantee CONTIGUOUS, ALIGNED and WRITEABLE
 * and therefore it is redundant to include those as well.
 *
 * BEHAVED == ALIGNED | WRITEABLE
 * CARRAY = CONTIGUOUS | BEHAVED
 * FARRAY = FORTRAN | BEHAVED
 *
 * FORTRAN can be set in the FLAGS to request a FORTRAN array.
 * Fortran arrays are always behaved (aligned,
 * notswapped, and writeable) and not (C) CONTIGUOUS (if > 1d).
 *
 * UPDATEIFCOPY flag sets this flag in the returned array if a copy is
 * made and the base argument points to the (possibly) misbehaved array.
 * When the new array is deallocated, the original array held in base
 * is updated with the contents of the new array.
 *
 * FORCECAST will cause a cast to occur regardless of whether or not
 * it is safe.
 */

/*NUMPY_API
 * steals a reference to descr -- accepts NULL
 */
NPY_NO_EXPORT PyObject *
PyArray_CheckFromAny(PyObject *op, PyArray_Descr *descr, int min_depth,
                     int max_depth, int requires, PyObject *context)
{
    PyObject *obj;
    if (requires & NOTSWAPPED) {
        if (!descr && PyArray_Check(op) &&
            !PyArray_ISNBO(PyArray_DESCR(op)->byteorder)) {
            descr = PyArray_DescrNew(PyArray_DESCR(op));
        }
        else if (descr && !PyArray_ISNBO(descr->byteorder)) {
            PyArray_DESCR_REPLACE(descr);
        }
        if (descr) {
            descr->byteorder = PyArray_NATIVE;
        }
    }

    obj = PyArray_FromAny(op, descr, min_depth, max_depth, requires, context);
    if (obj == NULL) {
        return NULL;
    }
    if ((requires & ELEMENTSTRIDES) &&
        !PyArray_ElementStrides(obj)) {
        PyObject *new;
        new = PyArray_NewCopy((PyArrayObject *)obj, PyArray_ANYORDER);
        Py_DECREF(obj);
        obj = new;
    }
    return obj;
}

/*NUMPY_API
 * steals reference to newtype --- acc. NULL
 */
NPY_NO_EXPORT PyObject *
PyArray_FromArray(PyArrayObject *arr, PyArray_Descr *newtype, int flags)
{

    PyArrayObject *ret = NULL;
    int itemsize;
    int copy = 0;
    int arrflags;
    PyArray_Descr *oldtype;
    char *msg = "cannot copy back to a read-only array";
    PyTypeObject *subtype;

    oldtype = PyArray_DESCR(arr);
    subtype = arr->ob_type;
    if (newtype == NULL) {
        newtype = oldtype; Py_INCREF(oldtype);
    }
    itemsize = newtype->elsize;
    if (itemsize == 0) {
        PyArray_DESCR_REPLACE(newtype);
        if (newtype == NULL) {
            return NULL;
        }
        newtype->elsize = oldtype->elsize;
        itemsize = newtype->elsize;
    }

    /*
     * Can't cast unless ndim-0 array, FORCECAST is specified
     * or the cast is safe.
     */
    if (!(flags & FORCECAST) && !PyArray_NDIM(arr) == 0 &&
        !PyArray_CanCastTo(oldtype, newtype)) {
        Py_DECREF(newtype);
        PyErr_SetString(PyExc_TypeError,
                        "array cannot be safely cast "  \
                        "to required type");
        return NULL;
    }

    /* Don't copy if sizes are compatible */
    if ((flags & ENSURECOPY) || PyArray_EquivTypes(oldtype, newtype)) {
        arrflags = arr->flags;
        copy = (flags & ENSURECOPY) ||
            ((flags & CONTIGUOUS) && (!(arrflags & CONTIGUOUS)))
            || ((flags & ALIGNED) && (!(arrflags & ALIGNED)))
            || (arr->nd > 1 &&
                ((flags & FORTRAN) && (!(arrflags & FORTRAN))))
            || ((flags & WRITEABLE) && (!(arrflags & WRITEABLE)));

        if (copy) {
            if ((flags & UPDATEIFCOPY) &&
                (!PyArray_ISWRITEABLE(arr))) {
                Py_DECREF(newtype);
                PyErr_SetString(PyExc_ValueError, msg);
                return NULL;
            }
            if ((flags & ENSUREARRAY)) {
                subtype = &PyArray_Type;
            }
            ret = (PyArrayObject *)
                PyArray_NewFromDescr(subtype, newtype,
                                     arr->nd,
                                     arr->dimensions,
                                     NULL, NULL,
                                     flags & FORTRAN,
                                     (PyObject *)arr);
            if (ret == NULL) {
                return NULL;
            }
            if (PyArray_CopyInto(ret, arr) == -1) {
                Py_DECREF(ret);
                return NULL;
            }
            if (flags & UPDATEIFCOPY)  {
                ret->flags |= UPDATEIFCOPY;
                ret->base = (PyObject *)arr;
                PyArray_FLAGS(ret->base) &= ~WRITEABLE;
                Py_INCREF(arr);
            }
        }
        /*
         * If no copy then just increase the reference
         * count and return the input
         */
        else {
            Py_DECREF(newtype);
            if ((flags & ENSUREARRAY) &&
                !PyArray_CheckExact(arr)) {
                Py_INCREF(arr->descr);
                ret = (PyArrayObject *)
                    PyArray_NewFromDescr(&PyArray_Type,
                                         arr->descr,
                                         arr->nd,
                                         arr->dimensions,
                                         arr->strides,
                                         arr->data,
                                         arr->flags,NULL);
                if (ret == NULL) {
                    return NULL;
                }
                ret->base = (PyObject *)arr;
            }
            else {
                ret = arr;
            }
            Py_INCREF(arr);
        }
    }

    /*
     * The desired output type is different than the input
     * array type and copy was not specified
     */
    else {
        if ((flags & UPDATEIFCOPY) &&
            (!PyArray_ISWRITEABLE(arr))) {
            Py_DECREF(newtype);
            PyErr_SetString(PyExc_ValueError, msg);
            return NULL;
        }
        if ((flags & ENSUREARRAY)) {
            subtype = &PyArray_Type;
        }
        ret = (PyArrayObject *)
            PyArray_NewFromDescr(subtype, newtype,
                                 arr->nd, arr->dimensions,
                                 NULL, NULL,
                                 flags & FORTRAN,
                                 (PyObject *)arr);
        if (ret == NULL) {
            return NULL;
        }
        if (PyArray_CastTo(ret, arr) < 0) {
            Py_DECREF(ret);
            return NULL;
        }
        if (flags & UPDATEIFCOPY)  {
            ret->flags |= UPDATEIFCOPY;
            ret->base = (PyObject *)arr;
            PyArray_FLAGS(ret->base) &= ~WRITEABLE;
            Py_INCREF(arr);
        }
    }
    return (PyObject *)ret;
}

/*NUMPY_API */
NPY_NO_EXPORT PyObject *
PyArray_FromStructInterface(PyObject *input)
{
    PyArray_Descr *thetype = NULL;
    char buf[40];
    PyArrayInterface *inter;
    PyObject *attr, *r;
    char endian = PyArray_NATBYTE;

    attr = PyObject_GetAttrString(input, "__array_struct__");
    if (attr == NULL) {
        PyErr_Clear();
        return Py_NotImplemented;
    }
    if (!PyCObject_Check(attr)) {
        goto fail;
    }
    inter = PyCObject_AsVoidPtr(attr);
    if (inter->two != 2) {
        goto fail;
    }
    if ((inter->flags & NOTSWAPPED) != NOTSWAPPED) {
        endian = PyArray_OPPBYTE;
        inter->flags &= ~NOTSWAPPED;
    }

    if (inter->flags & ARR_HAS_DESCR) {
        if (PyArray_DescrConverter(inter->descr, &thetype) == PY_FAIL) {
            thetype = NULL;
            PyErr_Clear();
        }
    }

    if (thetype == NULL) {
        PyOS_snprintf(buf, sizeof(buf),
                "%c%c%d", endian, inter->typekind, inter->itemsize);
        if (!(thetype=_array_typedescr_fromstr(buf))) {
            Py_DECREF(attr);
            return NULL;
        }
    }

    r = PyArray_NewFromDescr(&PyArray_Type, thetype,
                             inter->nd, inter->shape,
                             inter->strides, inter->data,
                             inter->flags, NULL);
    Py_INCREF(input);
    PyArray_BASE(r) = input;
    Py_DECREF(attr);
    PyArray_UpdateFlags((PyArrayObject *)r, UPDATE_ALL);
    return r;

 fail:
    PyErr_SetString(PyExc_ValueError, "invalid __array_struct__");
    Py_DECREF(attr);
    return NULL;
}

#define PyIntOrLong_Check(obj) (PyInt_Check(obj) || PyLong_Check(obj))

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_FromInterface(PyObject *input)
{
    PyObject *attr = NULL, *item = NULL;
    PyObject *tstr = NULL, *shape = NULL;
    PyObject *inter = NULL;
    PyObject *base = NULL;
    PyArrayObject *ret;
    PyArray_Descr *type=NULL;
    char *data;
    Py_ssize_t buffer_len;
    int res, i, n;
    intp dims[MAX_DIMS], strides[MAX_DIMS];
    int dataflags = BEHAVED;

    /* Get the memory from __array_data__ and __array_offset__ */
    /* Get the shape */
    /* Get the typestring -- ignore array_descr */
    /* Get the strides */

    inter = PyObject_GetAttrString(input, "__array_interface__");
    if (inter == NULL) {
        PyErr_Clear();
        return Py_NotImplemented;
    }
    if (!PyDict_Check(inter)) {
        Py_DECREF(inter);
        return Py_NotImplemented;
    }
    shape = PyDict_GetItemString(inter, "shape");
    if (shape == NULL) {
        Py_DECREF(inter);
        return Py_NotImplemented;
    }
    tstr = PyDict_GetItemString(inter, "typestr");
    if (tstr == NULL) {
        Py_DECREF(inter);
        return Py_NotImplemented;
    }

    attr = PyDict_GetItemString(inter, "data");
    base = input;
    if ((attr == NULL) || (attr==Py_None) || (!PyTuple_Check(attr))) {
        if (attr && (attr != Py_None)) {
            item = attr;
        }
        else {
            item = input;
        }
        res = PyObject_AsWriteBuffer(item, (void **)&data, &buffer_len);
        if (res < 0) {
            PyErr_Clear();
            res = PyObject_AsReadBuffer(
                    item, (const void **)&data, &buffer_len);
            if (res < 0) {
                goto fail;
            }
            dataflags &= ~WRITEABLE;
        }
        attr = PyDict_GetItemString(inter, "offset");
        if (attr) {
            longlong num = PyLong_AsLongLong(attr);
            if (error_converting(num)) {
                PyErr_SetString(PyExc_TypeError,
                                "offset "\
                                "must be an integer");
                goto fail;
            }
            data += num;
        }
        base = item;
    }
    else {
        PyObject *dataptr;
        if (PyTuple_GET_SIZE(attr) != 2) {
            PyErr_SetString(PyExc_TypeError,
                            "data must return "     \
                            "a 2-tuple with (data pointer "\
                            "integer, read-only flag)");
            goto fail;
        }
        dataptr = PyTuple_GET_ITEM(attr, 0);
        if (PyString_Check(dataptr)) {
            res = sscanf(PyString_AsString(dataptr),
                         "%p", (void **)&data);
            if (res < 1) {
                PyErr_SetString(PyExc_TypeError,
                                "data string cannot be " \
                                "converted");
                goto fail;
            }
        }
        else if (PyIntOrLong_Check(dataptr)) {
            data = PyLong_AsVoidPtr(dataptr);
        }
        else {
            PyErr_SetString(PyExc_TypeError, "first element " \
                            "of data tuple must be integer" \
                            " or string.");
            goto fail;
        }
        if (PyObject_IsTrue(PyTuple_GET_ITEM(attr,1))) {
            dataflags &= ~WRITEABLE;
        }
    }
    attr = tstr;
    if (!PyString_Check(attr)) {
        PyErr_SetString(PyExc_TypeError, "typestr must be a string");
        goto fail;
    }
    type = _array_typedescr_fromstr(PyString_AS_STRING(attr));
    if (type == NULL) {
        goto fail;
    }
    attr = shape;
    if (!PyTuple_Check(attr)) {
        PyErr_SetString(PyExc_TypeError, "shape must be a tuple");
        Py_DECREF(type);
        goto fail;
    }
    n = PyTuple_GET_SIZE(attr);
    for (i = 0; i < n; i++) {
        item = PyTuple_GET_ITEM(attr, i);
        dims[i] = PyArray_PyIntAsIntp(item);
        if (error_converting(dims[i])) {
            break;
        }
    }

    ret = (PyArrayObject *)PyArray_NewFromDescr(&PyArray_Type, type,
                                                n, dims,
                                                NULL, data,
                                                dataflags, NULL);
    if (ret == NULL) {
        return NULL;
    }
    Py_INCREF(base);
    ret->base = base;

    attr = PyDict_GetItemString(inter, "strides");
    if (attr != NULL && attr != Py_None) {
        if (!PyTuple_Check(attr)) {
            PyErr_SetString(PyExc_TypeError,
                            "strides must be a tuple");
            Py_DECREF(ret);
            return NULL;
        }
        if (n != PyTuple_GET_SIZE(attr)) {
            PyErr_SetString(PyExc_ValueError,
                            "mismatch in length of "\
                            "strides and shape");
            Py_DECREF(ret);
            return NULL;
        }
        for (i = 0; i < n; i++) {
            item = PyTuple_GET_ITEM(attr, i);
            strides[i] = PyArray_PyIntAsIntp(item);
            if (error_converting(strides[i])) {
                break;
            }
        }
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        memcpy(ret->strides, strides, n*sizeof(intp));
    }
    else PyErr_Clear();
    PyArray_UpdateFlags(ret, UPDATE_ALL);
    Py_DECREF(inter);
    return (PyObject *)ret;

 fail:
    Py_XDECREF(inter);
    return NULL;
}

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_FromArrayAttr(PyObject *op, PyArray_Descr *typecode, PyObject *context)
{
    PyObject *new;
    PyObject *array_meth;

    array_meth = PyObject_GetAttrString(op, "__array__");
    if (array_meth == NULL) {
        PyErr_Clear();
        return Py_NotImplemented;
    }
    if (context == NULL) {
        if (typecode == NULL) {
            new = PyObject_CallFunction(array_meth, NULL);
        }
        else {
            new = PyObject_CallFunction(array_meth, "O", typecode);
        }
    }
    else {
        if (typecode == NULL) {
            new = PyObject_CallFunction(array_meth, "OO", Py_None, context);
            if (new == NULL && PyErr_ExceptionMatches(PyExc_TypeError)) {
                PyErr_Clear();
                new = PyObject_CallFunction(array_meth, "");
            }
        }
        else {
            new = PyObject_CallFunction(array_meth, "OO", typecode, context);
            if (new == NULL && PyErr_ExceptionMatches(PyExc_TypeError)) {
                PyErr_Clear();
                new = PyObject_CallFunction(array_meth, "O", typecode);
            }
        }
    }
    Py_DECREF(array_meth);
    if (new == NULL) {
        return NULL;
    }
    if (!PyArray_Check(new)) {
        PyErr_SetString(PyExc_ValueError,
                        "object __array__ method not "  \
                        "producing an array");
        Py_DECREF(new);
        return NULL;
    }
    return new;
}

/*NUMPY_API
* new reference -- accepts NULL for mintype
*/
NPY_NO_EXPORT PyArray_Descr *
PyArray_DescrFromObject(PyObject *op, PyArray_Descr *mintype)
{
    return _array_find_type(op, mintype, MAX_DIMS);
}

/* These are also old calls (should use PyArray_NewFromDescr) */

/* They all zero-out the memory as previously done */

/* steals reference to descr -- and enforces native byteorder on it.*/
/*NUMPY_API
  Like FromDimsAndData but uses the Descr structure instead of typecode
  as input.
*/
NPY_NO_EXPORT PyObject *
PyArray_FromDimsAndDataAndDescr(int nd, int *d,
                                PyArray_Descr *descr,
                                char *data)
{
    PyObject *ret;
    int i;
    intp newd[MAX_DIMS];
    char msg[] = "PyArray_FromDimsAndDataAndDescr: use PyArray_NewFromDescr.";

    if (DEPRECATE(msg) < 0) {
        return NULL;
    }
    if (!PyArray_ISNBO(descr->byteorder))
        descr->byteorder = '=';
    for (i = 0; i < nd; i++) {
        newd[i] = (intp) d[i];
    }
    ret = PyArray_NewFromDescr(&PyArray_Type, descr,
                               nd, newd,
                               NULL, data,
                               (data ? CARRAY : 0), NULL);
    return ret;
}

/*NUMPY_API
  Construct an empty array from dimensions and typenum
*/
NPY_NO_EXPORT PyObject *
PyArray_FromDims(int nd, int *d, int type)
{
    PyObject *ret;
    char msg[] = "PyArray_FromDims: use PyArray_SimpleNew.";

    if (DEPRECATE(msg) < 0) {
        return NULL;
    }
    ret = PyArray_FromDimsAndDataAndDescr(nd, d,
                                          PyArray_DescrFromType(type),
                                          NULL);
    /*
     * Old FromDims set memory to zero --- some algorithms
     * relied on that.  Better keep it the same. If
     * Object type, then it's already been set to zero, though.
     */
    if (ret && (PyArray_DESCR(ret)->type_num != PyArray_OBJECT)) {
        memset(PyArray_DATA(ret), 0, PyArray_NBYTES(ret));
    }
    return ret;
}

/* end old calls */

/*NUMPY_API
 * This is a quick wrapper around PyArray_FromAny(op, NULL, 0, 0, ENSUREARRAY)
 * that special cases Arrays and PyArray_Scalars up front
 * It *steals a reference* to the object
 * It also guarantees that the result is PyArray_Type
 * Because it decrefs op if any conversion needs to take place
 * so it can be used like PyArray_EnsureArray(some_function(...))
 */
NPY_NO_EXPORT PyObject *
PyArray_EnsureArray(PyObject *op)
{
    PyObject *new;

    if (op == NULL) {
        return NULL;
    }
    if (PyArray_CheckExact(op)) {
        return op;
    }
    if (PyArray_Check(op)) {
        new = PyArray_View((PyArrayObject *)op, NULL, &PyArray_Type);
        Py_DECREF(op);
        return new;
    }
    if (PyArray_IsScalar(op, Generic)) {
        new = PyArray_FromScalar(op, NULL);
        Py_DECREF(op);
        return new;
    }
    new = PyArray_FromAny(op, NULL, 0, 0, ENSUREARRAY, NULL);
    Py_DECREF(op);
    return new;
}

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_EnsureAnyArray(PyObject *op)
{
    if (op && PyArray_Check(op)) {
        return op;
    }
    return PyArray_EnsureArray(op);
}

/*NUMPY_API
 * Copy an Array into another array -- memory must not overlap
 * Does not require src and dest to have "broadcastable" shapes
 * (only the same number of elements).
 */
NPY_NO_EXPORT int
PyArray_CopyAnyInto(PyArrayObject *dest, PyArrayObject *src)
{
    int elsize, simple;
    PyArrayIterObject *idest, *isrc;
    void (*myfunc)(char *, intp, char *, intp, intp, int);
    NPY_BEGIN_THREADS_DEF;

    if (!PyArray_EquivArrTypes(dest, src)) {
        return PyArray_CastAnyTo(dest, src);
    }
    if (!PyArray_ISWRITEABLE(dest)) {
        PyErr_SetString(PyExc_RuntimeError,
                "cannot write to array");
        return -1;
    }
    if (PyArray_SIZE(dest) != PyArray_SIZE(src)) {
        PyErr_SetString(PyExc_ValueError,
                "arrays must have the same number of elements"
                " for copy");
        return -1;
    }

    simple = ((PyArray_ISCARRAY_RO(src) && PyArray_ISCARRAY(dest)) ||
            (PyArray_ISFARRAY_RO(src) && PyArray_ISFARRAY(dest)));
    if (simple) {
        /* Refcount note: src and dest have the same size */
        PyArray_INCREF(src);
        PyArray_XDECREF(dest);
        NPY_BEGIN_THREADS;
        memcpy(dest->data, src->data, PyArray_NBYTES(dest));
        NPY_END_THREADS;
        return 0;
    }

    if (PyArray_SAMESHAPE(dest, src)) {
        int swap;

        if (PyArray_SAFEALIGNEDCOPY(dest) && PyArray_SAFEALIGNEDCOPY(src)) {
            myfunc = _strided_byte_copy;
        }
        else {
            myfunc = _unaligned_strided_byte_copy;
        }
        swap = PyArray_ISNOTSWAPPED(dest) != PyArray_ISNOTSWAPPED(src);
        return _copy_from_same_shape(dest, src, myfunc, swap);
    }

    /* Otherwise we have to do an iterator-based copy */
    idest = (PyArrayIterObject *)PyArray_IterNew((PyObject *)dest);
    if (idest == NULL) {
        return -1;
    }
    isrc = (PyArrayIterObject *)PyArray_IterNew((PyObject *)src);
    if (isrc == NULL) {
        Py_DECREF(idest);
        return -1;
    }
    elsize = dest->descr->elsize;
    /* Refcount note: src and dest have the same size */
    PyArray_INCREF(src);
    PyArray_XDECREF(dest);
    NPY_BEGIN_THREADS;
    while(idest->index < idest->size) {
        memcpy(idest->dataptr, isrc->dataptr, elsize);
        PyArray_ITER_NEXT(idest);
        PyArray_ITER_NEXT(isrc);
    }
    NPY_END_THREADS;
    Py_DECREF(idest);
    Py_DECREF(isrc);
    return 0;
}

/*NUMPY_API
 * Copy an Array into another array -- memory must not overlap.
 */
NPY_NO_EXPORT int
PyArray_CopyInto(PyArrayObject *dest, PyArrayObject *src)
{
    return _array_copy_into(dest, src, 1);
}


/*NUMPY_API
  PyArray_CheckAxis
*/
NPY_NO_EXPORT PyObject *
PyArray_CheckAxis(PyArrayObject *arr, int *axis, int flags)
{
    PyObject *temp1, *temp2;
    int n = arr->nd;

    if ((*axis >= MAX_DIMS) || (n==0)) {
        if (n != 1) {
            temp1 = PyArray_Ravel(arr,0);
            if (temp1 == NULL) {
                *axis = 0;
                return NULL;
            }
            *axis = PyArray_NDIM(temp1)-1;
        }
        else {
            temp1 = (PyObject *)arr;
            Py_INCREF(temp1);
            *axis = 0;
        }
        if (!flags) {
            return temp1;
        }
    }
    else {
        temp1 = (PyObject *)arr;
        Py_INCREF(temp1);
    }
    if (flags) {
        temp2 = PyArray_CheckFromAny((PyObject *)temp1, NULL,
                                     0, 0, flags, NULL);
        Py_DECREF(temp1);
        if (temp2 == NULL) {
            return NULL;
        }
    }
    else {
        temp2 = (PyObject *)temp1;
    }
    n = PyArray_NDIM(temp2);
    if (*axis < 0) {
        *axis += n;
    }
    if ((*axis < 0) || (*axis >= n)) {
        PyErr_Format(PyExc_ValueError,
                     "axis(=%d) out of bounds", *axis);
        Py_DECREF(temp2);
        return NULL;
    }
    return temp2;
}

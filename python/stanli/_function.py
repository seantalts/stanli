"""Python/NumPy adapter for the native, value-only Stan function interface."""
import ctypes
from collections.abc import Mapping

import numpy as np

from . import _lib, _resolve_program


_INT_MIN = np.iinfo(np.intc).min
_INT_MAX = np.iinfo(np.intc).max


class _Argument(ctypes.Structure):
    """Mirrors stanli_function_argument in stanli/function.hpp."""
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("is_int", ctypes.c_int),
        ("reals", ctypes.POINTER(ctypes.c_double)),
        ("ints", ctypes.POINTER(ctypes.c_int)),
        ("size", ctypes.c_size_t),
        ("dims", ctypes.POINTER(ctypes.c_int64)),
        ("dim_size", ctypes.c_size_t),
    ]


_ResultWriter = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
    ctypes.POINTER(ctypes.c_double), ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_int), ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_int64), ctypes.c_size_t)


@_ResultWriter
def _write_result(context, is_int, reals, real_size, ints, int_size,
                  dims, dim_size):
    """Copy while the runtime owns the buffers; never raise across ctypes."""
    result = ctypes.cast(context, ctypes.POINTER(ctypes.py_object)).contents.value
    try:
        data, size = (ints, int_size) if is_int else (reals, real_size)
        if dim_size == 0:
            if size != 1:
                raise RuntimeError("Stan function returned an invalid scalar")
            result["value"] = data[0]
        else:
            shape = tuple(dims[i] for i in range(dim_size))
            if size:
                values = np.ctypeslib.as_array(data, shape=(size,)).copy()
            else:
                values = np.empty(0, dtype=np.intc if is_int else np.float64)
            result["value"] = values.reshape(shape, order="F")
        return 0
    except BaseException as exc:
        result["error"] = exc
        return 1


_lib.stanli_function_new_from_mir.restype = ctypes.c_void_p
_lib.stanli_function_new_from_mir.argtypes = [
    ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.stanli_function_free.argtypes = [ctypes.c_void_p]
_lib.stanli_function_free.restype = None
_lib.stanli_function_call_values.restype = ctypes.c_int
_lib.stanli_function_call_values.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(_Argument), ctypes.c_size_t,
    _ResultWriter, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]


class Function:
    """A callable, value-only Stan user-defined function, compiled once.

    Supply its name (or a resolved overload such as ``f(real,vector)``)
    and one of ``stan_file``, ``stan_code``, or cached ``mir``::

        affine = stanli.Function("affine", stan_code=source)
        affine(x=[1, 2, 4], a=2.5, b=-1)  # numpy array([1.5, 4., 9.])

    Calls accept named keyword arguments or one mapping. Real/int scalars
    return Python float/int; containers return independent NumPy arrays
    with their logical dimensions preserved. Inputs can be numeric scalars,
    rectangular lists, or NumPy arrays (including non-contiguous views).
    Integer inputs must fit Stan's 32-bit integer type; they promote to real
    formals. Empty lists default to real, so use an integer-dtype NumPy array
    for an empty integer argument. Complex values are not supported.

    Like the native Function API, this evaluates pure, value-returning UDFs
    using doubles, not autodiff. RNG, void, and _lp functions are outside
    this interface. No C++ compiler is needed.
    ``include_paths`` uses the same directory search order as ``Model``.
    """

    def __init__(self, name, *, stan_file=None, stan_code=None, mir=None,
                 include_paths=None):
        self._function = None
        if not isinstance(name, str) or not name or "\0" in name:
            raise ValueError("function name must be a nonempty string without NUL")
        self.name = name
        mir, _ = _resolve_program(stan_file, stan_code, mir, None, include_paths)
        err = ctypes.create_string_buffer(8192)
        self._function = _lib.stanli_function_new_from_mir(
            mir.encode(), name.encode(), err, len(err))
        if not self._function:
            raise RuntimeError(err.value.decode())

    def __del__(self):
        if getattr(self, "_function", None):
            _lib.stanli_function_free(self._function)
            self._function = None

    def __call__(self, arguments=None, /, **kwargs):
        if arguments is None:
            arguments = kwargs
        elif kwargs:
            raise TypeError("supply one argument mapping or keyword arguments, not both")
        if not isinstance(arguments, Mapping):
            raise TypeError("Stan function arguments must be a mapping or keywords")

        native = (_Argument * len(arguments))()
        # ctypes borrows all these buffers until the call (including the
        # result callback) finishes. Never let conversion temporaries die.
        buffers = []
        for i, (name, value) in enumerate(arguments.items()):
            if not isinstance(name, str) or not name or "\0" in name:
                raise ValueError("argument names must be nonempty strings without NUL")
            # Exact built-ins need no NumPy shape/dtype discovery. Keep bool,
            # subclasses, and NumPy scalars on the established conversion path
            # so their array protocols and rejection behavior are unchanged.
            # Storage remains call-local, including during reentrant callbacks.
            if type(value) is float:
                scalar = ctypes.c_double(value)
                encoded_name = name.encode()
                native[i] = _Argument(encoded_name, 0, ctypes.pointer(scalar),
                                      None, 1, None, 0)
                buffers.extend((scalar, encoded_name))
                continue
            if type(value) is int:
                if not _INT_MIN <= value <= _INT_MAX:
                    raise OverflowError(f"argument '{name}' does not fit a Stan integer")
                scalar = ctypes.c_int(value)
                encoded_name = name.encode()
                native[i] = _Argument(encoded_name, 1, None,
                                      ctypes.pointer(scalar), 1, None, 0)
                buffers.extend((scalar, encoded_name))
                continue
            if isinstance(value, (int, np.integer)) and not (
                    _INT_MIN <= value <= _INT_MAX):
                raise OverflowError(f"argument '{name}' does not fit a Stan integer")
            array = np.asarray(value)
            is_int = array.dtype.kind in "iu"
            if is_int:
                if array.size and (array.min() < _INT_MIN or
                                   array.max() > _INT_MAX):
                    raise OverflowError(f"argument '{name}' does not fit a Stan integer")
            elif array.dtype.kind != "f":
                raise TypeError(f"argument '{name}' must contain real or integer numbers")
            flat = np.ascontiguousarray(array.ravel(order="F"),
                                        dtype=np.intc if is_int else np.float64)
            dims = (ctypes.c_int64 * array.ndim)(*array.shape)
            encoded_name = name.encode()
            native[i] = _Argument(
                encoded_name, is_int,
                None if is_int else flat.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
                flat.ctypes.data_as(ctypes.POINTER(ctypes.c_int)) if is_int else None,
                flat.size, dims, array.ndim)
            buffers.extend((flat, dims, encoded_name))

        result = {"value": None, "error": None}
        context = ctypes.py_object(result)
        err = ctypes.create_string_buffer(8192)
        rc = _lib.stanli_function_call_values(
            self._function, native, len(native), _write_result,
            ctypes.byref(context), err, len(err))
        if result["error"] is not None:
            raise result["error"]
        if rc:
            raise RuntimeError(err.value.decode())
        return result["value"]

    def __repr__(self):
        return f"<stanli.Function {self.name}>"

    def __reduce__(self):
        # Duplicating a raw handle would double-free it; unpickling one in
        # another process would dereference an unrelated address.
        raise TypeError("Stan function handles cannot be copied or pickled; cache MIR instead")

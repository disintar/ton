from python_ton import PyCellBuilder, PyCellSlice, PyCell, PyDict, parseStringToCell, PyEmulator


class CellSlice:
    def __init__(self, old_cell_slice):
        self.cell_slice = old_cell_slice

        def fix_run(s):
            def f(*args, **kwargs):
                return getattr(self.cell_slice, s)(*args, **kwargs)

            return f

        for method in old_cell_slice.__dir__():
            if '__' != method[:2] and method not in ['load_int', 'load_uint', 'to_boc',
                                                     'dump_as_tlb', 'load_tlb', 'bselect', 'fetch_ref']:
                setattr(self, method, fix_run(method))

    def load_int(self, n):
        return int(self.cell_slice.load_int(n))

    def load_uint(self, n):
        return int(self.cell_slice.load_uint(n))

    def load_var_uint(self, n, sgnd=True):
        return int(self.cell_slice.load_var_integer_str(n, sgnd))

    def load_grams(self):
        return int(self.cell_slice.load_var_integer_str(4, False))

    def to_boc(self):
        return self.cell_slice.to_boc()

    def dump_as_tlb(self, a):
        return self.cell_slice.dump_as_tlb(a)

    def load_tlb(self, t):
        return CellSlice(self.cell_slice.load_tlb(t))

    def bselect(self, a, b):
        return int(self.cell_slice.bselect(a, str(b)))

    def fetch_ref(self):
        return CellSlice(self.cell_slice.fetch_ref())

    def __repr__(self):
        return self.cell_slice.__repr__()


class CellBuilder:
    def __init__(self):
        self.builder = PyCellBuilder()

        def fix_run(s):

            def f(*args, **kwargs):
                getattr(self.builder, s)(*args, **kwargs)
                return self

            return f

        for method in self.builder.__dir__():
            if '__' != method[:2] and method not in ['store_ref', 'store_builder', 'dump',
                                                     'store_slice', 'store_var_integer', 'to_boc', 'dump_as_tlb']:
                setattr(self, method, fix_run(method))

    def store_ref(self, old_cell):
        self.builder.store_ref(old_cell.cell)
        return self

    def store_grams(self, n):
        self.builder.store_grams_str(str(n))
        return self

    def to_boc(self):
        return self.builder.to_boc()

    def store_slice(self, cell_slice):
        self.builder.store_slice(cell_slice.cell_slice)
        return self

    def store_builder(self, b):
        self.builder.store_builder(b.builder)
        return self

    def store_uint(self, a, b):
        self.store_uint_str(str(a), b)
        return self

    def store_int(self, a, b):
        self.store_int_str(str(a), b)
        return self

    def store_addr(slef, a):
        self.builder.store_address(a)
        return self

    def store_var_uint(self, a, b):
        self.builder.store_var_integer(str(a), b, False)
        return self

    def store_var_int(self, a, b):
        self.builder.store_var_integer(str(a), b, True)
        return self

    def store_var_integer(self, a, b, c=False):
        self.builder.store_var_integer(str(a), b, c)
        return self

    def end_cell(self):
        return Cell(self.builder.get_cell())

    def dump(self):
        return self.builder.dump()

    def dump_as_tlb(self, a):
        return self.builder.dump_as_tlb(a)

    def __repr__(self):
        return self.builder.__repr__()


class Cell:
    def __init__(self, old_cell):
        self.cell = old_cell

        def fix_run(s):
            def f(*args, **kwargs):
                result = getattr(self.cell, s)(*args, **kwargs)
                return self

            return f

        for method in old_cell.__dir__():
            if '__' != method[:2] and method not in ['begin_parse', 'store_ref', 'get_hash', 'dump', 'to_boc',
                                                     'dump_as_tlb', 'is_null']:
                setattr(self, method, fix_run(method))

    def get_hash(self):
        return self.cell.get_hash()

    def to_boc(self):
        return self.cell.to_boc()

    def begin_parse(self):
        return CellSlice(self.cell.begin_parse())

    def dump_as_tlb(self, a):
        return self.cell.dump_as_tlb(a)

    def is_null(self):
        return self.cell.is_null()

    def dump(self):
        return self.cell.dump()

    def __repr__(self):
        return self.cell.__repr__()


class VmDict:
    def __init__(self, key_len, signed=False, cs_root=None):
        self.key_len = key_len
        self.signed = signed
        if cs_root is None:
            self.dict = PyDict(key_len, signed)
        else:
            self.dict = PyDict(key_len, signed, cs_root.cell_slice)

        def fix_run(s):
            def f(*args, **kwargs):
                return getattr(self.builder, s)(*args, **kwargs)

            return f

        for method in self.dict.__dir__():
            if '__' != method[:2] and method not in ['set_str', 'set_ref_str', 'set_builder_str',
                                                     'lookup_str', 'lookup_ref_str',
                                                     'lookup_delete_str', 'lookup_delete_ref_str',
                                                     'lookup_nearest_key', 'get_minmax_key', 'get_minmax_key_ref']:
                setattr(self, method, fix_run(method))

    def set(self, key, value, mode="set", key_len=0, signed=False):
        if not isinstance(value, CellSlice):
            raise ValueError(f"CellSlice needed")

        self.dict.set_str(str(key), value.cell_slice, mode, key_len, signed)
        return self

    def lookup_nearest_key(self, key, fetch_next=True, allow_eq=False, inver_first=False, key_len=0, signed=-1):
        key, value = self.dict.lookup_nearest_key(str(key), fetch_next, allow_eq, inver_first, key_len, signed)
        return int(key), value

    def get_minmax_key(self, fetch_max=True, inver_first=False, key_len=0, signed=-1):
        key, value = self.dict.get_minmax_key(fetch_max, inver_first, key_len, signed)
        return int(key), value

    def get_minmax_key_ref(self, fetch_max=True, inver_first=False, key_len=0, signed=-1):
        key, value = self.dict.get_minmax_key_ref(fetch_max, inver_first, key_len, signed)
        return int(key), value

    def set_ref(self, key, value, mode="set", key_len=0, signed=-1):
        if not isinstance(value, Cell):
            raise ValueError(f"Cell needed")

        self.dict.set_ref_str(str(key), value.cell, mode, key_len, signed)
        return self

    def set_builder(self, key, value, mode="set", key_len=0, signed=-1):
        if not isinstance(value, CellBuilder):
            raise ValueError(f"CellBuilder needed")

        self.dict.set_builder_str(str(key), value.builder, mode, key_len, signed)
        return self

    def lookup(self, key, key_len=0, signed=-1):
        return CellSlice(self.dict.lookup_str(str(key), key_len, signed))

    def lookup_delete(self, key, key_len=0, signed=-1):
        return CellSlice(self.dict.lookup_delete_str(str(key), key_len, signed))

    def lookup_ref(self, key, key_len=0, signed=-1):
        return Cell(self.dict.lookup_ref_str(str(key), key_len, signed))

    def lookup_delete_ref(self, key, key_len=0, signed=-1):
        return Cell(self.dict.lookup_delete_ref_str(str(key), key_len, signed))

    def get_cell(self):
        return Cell(self.dict.get_pycell())

    def __repr__(self):
        return self.dict.__repr__()

    def get_iter(self, direction=False):
        key, value = self.get_minmax_key(direction)
        yield (key, value)

        while True:
            try:
                key, value = self.lookup_nearest_key(key, not direction)
                yield (key, value)
            except Exception as e:
                return

    def __iter__(self):
        return self.get_iter(False)

    def __reversed__(self):
        return self.get_iter(True)


def parse_boc(b):
    return Cell(parseStringToCell(b))


def begin_cell():
    return CellBuilder()

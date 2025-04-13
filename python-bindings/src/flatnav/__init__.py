import sys 
from ._core import (
    MetricType,
    data_type,
    entry_policy,
    __version__,
    __doc__
)

class _DataTypeModule:
    from ._core.data_type import DataType

class _EntryPolicyModule:
    from ._core.entry_policy import EntryPolicy

class _IndexModule:
    from ._core.index import (
        IndexL2Float,
        IndexIPFloat,
        IndexL2Uint8,
        IndexIPUint8,
        IndexL2Int8,
        IndexIPInt8,
        create,
    )


index = _IndexModule
sys.modules['flatnav.index'] = _IndexModule
sys.modules['flatnav.data_type'] = _DataTypeModule
sys.modules['flatnav.entry_policy'] = _EntryPolicyModule

__all__ = [
    'MetricType',
    'data_type',
    'entry_policy',
    'index',
    '__version__',
    '__doc__'
]
import decidb
import sys


def test_version():
    assert decidb.__version__ != "0.0.0"


def test_formatted_python_version():
    formatted_python_version = f"{sys.version_info.major}.{sys.version_info.minor}"
    assert decidb.__formatted_python_version__ == formatted_python_version

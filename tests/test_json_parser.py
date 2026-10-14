import sys
import os
from pathlib import Path

build_dir = str(Path(__file__).resolve().parents[1] / "build")
sys.path.insert(0, build_dir)

# Add DLL directories for Windows
if hasattr(os, 'add_dll_directory'):
    os.add_dll_directory(build_dir)
    os.add_dll_directory("C:/vcpkg/installed/x64-windows/bin")

from PyQuantLib import JsonDocument, JsonParseError, init_parser, get_simd_arch, simd_arch_to_string

def test_simple_values():
    """Test parsing simple JSON values"""
    print("Testing simple values...")

    # Null
    doc = JsonDocument.parse("null")
    assert doc.is_valid()
    assert doc.root().is_null()
    print("  [OK] null")

    # Boolean
    doc = JsonDocument.parse("true")
    assert doc.root().is_bool()
    assert doc.root().as_bool() == True
    print("  [OK] boolean (true)")

    doc = JsonDocument.parse("false")
    assert doc.root().as_bool() == False
    print("  [OK] boolean (false)")

    # Number
    doc = JsonDocument.parse("42")
    assert doc.root().is_number()
    assert doc.root().as_number() == 42.0
    print("  [OK] integer")

    doc = JsonDocument.parse("3.14159")
    assert abs(doc.root().as_number() - 3.14159) < 0.00001
    print("  [OK] float")

    doc = JsonDocument.parse("-123.456e-2")
    assert abs(doc.root().as_number() - (-1.23456)) < 0.00001
    print("  [OK] scientific notation")

    # String
    doc = JsonDocument.parse('"hello world"')
    assert doc.root().is_string()
    assert doc.root().as_string() == "hello world"
    print("  [OK] string")

    doc = JsonDocument.parse(r'"escaped: \n \t \\ \" "')
    assert doc.root().is_string()
    print("  [OK] escaped string")

def test_arrays():
    """Test parsing JSON arrays"""
    print("\nTesting arrays...")

    # Empty array
    doc = JsonDocument.parse("[]")
    assert doc.root().is_array()
    assert len(doc.root().as_array()) == 0
    print("  [OK] empty array")

    # Simple array
    doc = JsonDocument.parse("[1, 2, 3, 4, 5]")
    assert doc.root().is_array()
    arr = doc.root().as_array()
    assert len(arr) == 5
    assert arr[0].as_number() == 1.0
    assert arr[4].as_number() == 5.0
    print("  [OK] number array")

    # Mixed array
    doc = JsonDocument.parse('[1, "two", true, null, 3.14]')
    arr = doc.root().as_array()
    assert len(arr) == 5
    assert arr[0].is_number()
    assert arr[1].is_string()
    assert arr[2].is_bool()
    assert arr[3].is_null()
    assert arr[4].is_number()
    print("  [OK] mixed array")

    # Nested array
    doc = JsonDocument.parse("[[1, 2], [3, 4], [5, 6]]")
    arr = doc.root().as_array()
    assert len(arr) == 3
    assert arr[0].is_array()
    assert len(arr[0].as_array()) == 2
    print("  [OK] nested array")

def test_objects():
    """Test parsing JSON objects"""
    print("\nTesting objects...")

    # Empty object
    doc = JsonDocument.parse("{}")
    assert doc.root().is_object()
    assert len(doc.root().as_object()) == 0
    print("  [OK] empty object")

    # Simple object
    doc = JsonDocument.parse('{"name": "Alice", "age": 30}')
    assert doc.root().is_object()
    obj = doc.root().as_object()
    assert len(obj) == 2
    assert obj["name"].as_string() == "Alice"
    assert obj["age"].as_number() == 30.0
    print("  [OK] simple object")

    # Nested object
    doc = JsonDocument.parse('{"person": {"name": "Bob", "address": {"city": "NYC"}}}')
    obj = doc.root().as_object()
    assert obj["person"].is_object()
    person = obj["person"].as_object()
    assert person["name"].as_string() == "Bob"
    assert person["address"].is_object()
    print("  [OK] nested object")

def test_complex_json():
    """Test parsing complex JSON structures"""
    print("\nTesting complex JSON...")

    json_str = '''
    {
        "users": [
            {"id": 1, "name": "Alice", "active": true},
            {"id": 2, "name": "Bob", "active": false},
            {"id": 3, "name": "Charlie", "active": true}
        ],
        "metadata": {
            "version": "1.0",
            "count": 3,
            "timestamp": 1234567890.123
        }
    }
    '''

    doc = JsonDocument.parse(json_str)
    root = doc.root().as_object()

    users = root["users"].as_array()
    assert len(users) == 3
    assert users[0].as_object()["name"].as_string() == "Alice"
    assert users[1].as_object()["active"].as_bool() == False
    assert users[2].as_object()["id"].as_number() == 3.0

    metadata = root["metadata"].as_object()
    assert metadata["version"].as_string() == "1.0"
    assert metadata["count"].as_number() == 3.0

    print("  [OK] complex nested structure")

def test_error_handling():
    """Test error handling"""
    print("\nTesting error handling...")

    test_cases = [
        ('', "empty document"),
        ('{', "unclosed object"),
        ('{"key"}', "missing colon"),
        ('[1, 2,]', "trailing comma"),
        ('nul', "incomplete null"),
        ('tru', "incomplete true"),
    ]

    for json_str, desc in test_cases:
        try:
            JsonDocument.parse(json_str)
            print(f"  [FAIL] {desc} (should have failed)")
        except Exception as e:
            print(f"  [OK] {desc}")

def test_whitespace_handling():
    """Test whitespace handling"""
    print("\nTesting whitespace handling...")

    # Various whitespace
    doc = JsonDocument.parse('  \n\t  {  "key"  :  "value"  }  \n  ')
    assert doc.root().is_object()
    assert doc.root().as_object()["key"].as_string() == "value"
    print("  [OK] whitespace handling")

def main():
    print("=" * 60)
    print("SIMD JSON Parser Test Suite")
    print("=" * 60)

    # Initialize parser and show SIMD capabilities
    init_parser()
    arch = get_simd_arch()
    arch_str = simd_arch_to_string(arch)
    print(f"\nSIMD Architecture: {arch_str}")
    print()

    # Run tests
    test_simple_values()
    test_arrays()
    test_objects()
    test_complex_json()
    test_error_handling()
    test_whitespace_handling()

    print("\n" + "=" * 60)
    print("All tests completed!")
    print("=" * 60)

if __name__ == "__main__":
    main()

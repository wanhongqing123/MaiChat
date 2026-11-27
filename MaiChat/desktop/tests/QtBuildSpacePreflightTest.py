"""The reduced disk budget applies only to an existing compatible macOS SDK lane."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

script = Path(__file__).resolve().parents[1] / 'scripts/build-qt.py'
spec = importlib.util.spec_from_file_location('qt_source_builder', script)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class QtBuildSpacePreflightTest(unittest.TestCase):
    def test_requires_same_source_prefix_compilers_architecture_and_ninja(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source, build, prefix = root / 'source', root / 'build', root / 'install'
            module = build / 'qtbase'
            module.mkdir(parents=True)
            source.mkdir()
            values = {'CMAKE_HOME_DIRECTORY': str(source / 'qtbase'),
                      'CMAKE_INSTALL_PREFIX': str(prefix), 'CMAKE_C_COMPILER': '/usr/bin/cc',
                      'CMAKE_CXX_COMPILER': '/usr/bin/c++', 'CMAKE_GENERATOR': 'Ninja',
                      'CMAKE_BUILD_TYPE': 'Release', 'CMAKE_OSX_ARCHITECTURES': 'arm64'}
            def check():
                return builder.is_configured_macos_sdk(source, build, prefix,
                    [('qtbase', 'locked-sha')], 'arm64', '/usr/bin/cc', '/usr/bin/c++')
            self.assertFalse(check())
            (module / 'build.ninja').write_text('# owned generated cache\n')
            def save():
                (module / 'CMakeCache.txt').write_text(''.join(
                    key + ':STRING=' + value + '\n' for key, value in values.items()))
            save()
            self.assertTrue(check())
            for key in values:
                before = values[key]
                values[key] = before + '-different'
                save()
                self.assertFalse(check(), key)
                values[key] = before
            save()
            self.assertFalse(builder.is_configured_macos_sdk(source, build, prefix,
                [('qtbase', 'locked-sha'), ('qtsvg', 'locked-sha')], 'arm64', '/usr/bin/cc', '/usr/bin/c++'))
            self.assertFalse(builder.is_configured_macos_sdk(source, build, prefix,
                [('qtbase', 'locked-sha')], 'arm64', None, None))


if __name__ == '__main__':
    unittest.main()

from conans import ConanFile, CMake, tools


class SweaterConan(ConanFile):
    name = "Sweater"
    version = "1.0.1"
    requires = 'Functionoid/[>=1.0.0]@microblink/master', 'ConfigEx/[>=1.0.0]@microblink/master'
    settings = 'os'
    license = "MIT"
    url = "https://github.com/microblink/functionoid"
    generators = "cmake"
    scm = {
        "type": "git",
        "url": "auto",
        "revision": "auto"
    }
    no_copy_source = True

    def requirements(self):
        if self.settings.os != 'iOS' and self.settings.os != 'Macos':
            self.requires('ConcurrentQueue/[>=1.0.0]@microblink/master')

    def package(self):
        self.copy("include/*.hpp")

    def package_id(self):
        self.info.header_only()


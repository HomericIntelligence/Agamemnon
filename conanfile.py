from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps


class ProjectAgamemnonConan(ConanFile):
    name = "projectagamemnon"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("cpp-httplib/0.18.3")
        self.requires("nlohmann_json/3.11.3")
        self.requires("libcurl/8.6.0")
        # OpenSSL is consumed directly by CMakeLists.txt (find_package(OpenSSL)
        # + OpenSSL::SSL / OpenSSL::Crypto). Without an explicit requirement it
        # was only pulled in transitively via libcurl, and CMakeDeps generated
        # incomplete OpenSSL targets whose 'ssl'/'crypto' library files could
        # not be located ("Library 'ssl' not found in package"). Declaring it as
        # a first-class dependency forces Conan to fully build and generate the
        # OpenSSL package so its CMake targets resolve. Version matches the pin
        # libcurl/8.6.0 selects from its "openssl/[>=1.1 <4]" range to avoid a
        # version clash.
        self.requires("openssl/3.6.2")
        self.requires("prometheus-cpp/1.2.4")
        # ADR-015: lock-free MPMC queue backing the ported HMAS work-stealing
        # scheduler / agent inboxes (matches Keystone's concurrentqueue pin).
        self.requires("concurrentqueue/1.0.4")

    def configure(self):
        # Use core serializer only; HTTP serving is embedded in cpp-httplib (no pull server)
        self.options["prometheus-cpp"].with_pull = False
        self.options["prometheus-cpp"].with_push = False
        self.options["prometheus-cpp"].with_compression = False

    def build_requirements(self):
        self.test_requires("gtest/1.14.0")

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()

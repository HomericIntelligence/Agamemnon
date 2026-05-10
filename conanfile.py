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
        self.requires("prometheus-cpp/1.2.4")

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

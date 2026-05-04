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
        self.requires("openssl/1.1.1w")

    def build_requirements(self):
        self.test_requires("gtest/1.14.0")

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()

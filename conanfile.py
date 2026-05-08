from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps


class webserverRecipe(ConanFile):
    name = "webserver"
    version = "1.0"
    package_type = "application"

    # Some metadata
    author = "paranoia986 2146000986@qq.com"
    url = "<Package recipe repository url here, for issues about the package>"
    description = "a simple webserver which is inspired by tinywebserver"
    topics = ("web","http","mysql","threadpool","lock","io_uring")

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"

    # Sources are located in the same place as this recipe, copy them to the recipe
    exports_sources = "CMakeLists.txt", "src/*"
    
    options = {
        "io_uring": [True, False]
    }
    default_options = {
        "io_uring": True
    }
    
    def requirements(self):
        # 添加 MySQL 客户端依赖
        self.requires("libmysqlclient/8.0.31")
        # 添加 yaml-cpp 依赖
        self.requires("yaml-cpp/0.8.0")
        
        # 添加 liburing 依赖
        if self.options.io_uring:
            self.requires("liburing/2.6")
        
    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["IO_URING"] = self.options.io_uring
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    

    

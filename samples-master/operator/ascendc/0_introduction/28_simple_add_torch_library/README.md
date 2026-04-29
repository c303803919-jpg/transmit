# torch.library注册自定义算子直调样例
本样例展示了如何使用PyTorch的torch.library机制注册自定义算子，并通过<<<>>>内核调用符调用核函数，以简单的Add算子为例，实现两个向量的逐元素相加。

## 目录结构介绍
```
├── 28_simple_add_torch_library
│   ├── CMakeLists.txt          // 编译工程文件
│   ├── add_custom_test.py      // PyTorch调用自定义算子的测试脚本
│   └── add_custom.asc          // Ascend C算子实现 & 自定义算子注册
```

## 代码实现介绍
- kernel实现  
  Add算子的数学表达式为：
  ```
  z = x + y
  ```
  计算逻辑是：Ascend C提供的矢量计算接口的操作元素都为LocalTensor，输入数据需要先搬运进片上存储，然后使用计算接口完成两个输入参数相加，得到最终结果，再搬出到外部存储上。

  Add算子的实现流程分为3个基本任务：CopyIn，Compute，CopyOut。CopyIn任务负责将Global Memory上的输入Tensor xGm和yGm搬运到Local Memory，分别存储在xLocal、yLocal，Compute任务负责对xLocal、yLocal执行加法操作，计算结果存储在zLocal中，CopyOut任务负责将输出数据从zLocal搬运至Global Memory上的输出Tensor zGm中。具体请参考[add_custom.asc](./add_custom.asc)。

- 自定义算子注册：

  本样例在add_custom.asc中定义了一个名为ascendc_ops的命名空间，并在其中注册了ascendc_add函数。

  PyTorch提供`TORCH_LIBRARY`宏作为自定义算子注册的核心接口，用于创建并初始化自定义算子库，注册后在Python侧可以通过`torch.ops.namespace.op_name`方式进行调用，例如：
  ```c++
  TORCH_LIBRARY(ascendc_ops, m) {
      m.def(ascendc_add"(Tensor x, Tensor y) -> Tensor");
  }
  ```

  `TORCH_LIBRARY_IMPL`用于将算子逻辑绑定到特定的DispatchKey（PyTorch设备调度标识）。针对NPU设备，需要将算子实现注册到PrivateUse1这一专属的DispatchKey上，例如：
  ```c++
  TORCH_LIBRARY_IMPL(ascendc_ops, PrivateUse1, m)
  {
      m.impl("ascendc_add", TORCH_FN(ascendc_ops::ascendc_add));
  }
  ```
  在ascendc_add函数中通过`c10_npu::getCurrentNPUStream()`函数获取当前NPU上的流，并通过内核调用符<<<>>>调用自定义的Kernel函数add_custom，在NPU上执行算子。

- Python测试脚本

  在add_custom_test.py中，首先通过`torch.ops.load_library`加载生成的自定义算子库，调用注册的ascendc_add函数，并通过对比NPU输出与CPU标准加法结果来验证自定义算子的数值正确性。

## 支持的产品型号
本样例支持如下产品型号：
- Atlas A2 训练系列产品/Atlas 800I A2 推理产品

## 运行样例算子
- 请参考与您当前使用的版本配套的[《Ascend Extension for PyTorch
软件安装指南》](https://www.hiascend.com/document/detail/zh/Pytorch/720/configandinstg/instg/insg_0001.html)，获取PyTorch和torch_npu详细的安装步骤。  

- 打开样例目录   
以命令行方式下载样例代码，master分支为例。
  ```bash
  cd ${git_clone_path}/samples/operator/ascendc/0_introduction/28_simple_add_torch_library
  ```

- 配置环境变量  
  请根据当前环境上CANN开发套件包的[安装方式](https://hiascend.com/document/redirect/CannCommunityInstSoftware)，选择对应配置环境变量的命令。
    - 默认路径，root用户安装CANN软件包
      ```bash
      export ASCEND_INSTALL_PATH=/usr/local/Ascend/ascend-toolkit/latest
      ```
    - 默认路径，非root用户安装CANN软件包
      ```bash
      export ASCEND_INSTALL_PATH=$HOME/Ascend/ascend-toolkit/latest
      ```
    - 指定路径install_path，安装CANN软件包
      ```bash
      export ASCEND_INSTALL_PATH=${install_path}/ascend-toolkit/latest
      ```
    配置安装路径后，执行以下命令统一配置环境变量。
    ```bash
    # 配置CANN环境变量
    source ${ASCEND_INSTALL_PATH}/bin/setenv.bash
    # 添加AscendC CMake Module搜索路径至环境变量
    export CMAKE_PREFIX_PATH=${ASCEND_INSTALL_PATH}/compiler/tikcpp/ascendc_kernel_cmake:$CMAKE_PREFIX_PATH
    ```

- 样例执行
  ```bash
  mkdir -p build && cd build;     # 创建并进入build目录
  cmake ..;make -j;               # 编译工程
  python3 ../add_custom_test.py   # 执行测试脚本
  ```

## 更新说明
| 时间       | 更新事项     |
| ---------- | ------------ |
| 2025/12/25 | 新增本readme |

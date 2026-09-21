// M6-11：engine/render 公共头的独立编译 smoke；每个头在生成的 TU 中单独包含，并检查
// Windows/D3D 宏没有被公共头传递进来。真正的断言在生成的 TU 内，这里只需提供入口。
int main()
{
    return 0;
}

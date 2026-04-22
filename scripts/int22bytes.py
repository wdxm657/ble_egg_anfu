import sys

def int_to_two_bytes_hex(num):
    # 确保在16位有符号整数范围内
    if num < -32768 or num > 32767:
        raise ValueError("整数超出16位有符号范围")
    # 通过 & 0xFFFF 获取低16位（对负数补码有效）
    val = num & 0xFFFF
    high = (val >> 8) & 0xFF
    low = val & 0xFF
    return high, low

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("用法: python py <水平角度> <垂直角度>")
        sys.exit(1)
    try:
        num = (float)(sys.argv[1]) * 10
        high, low = int_to_two_bytes_hex((int)(num))
        num2 = (float)(sys.argv[2]) * 10
        high2, low2 = int_to_two_bytes_hex((int)(num2))
        print(f"01012001060001{low:02X}{high:02X}{low2:02X}{high2:02X}02")
        # 也可以打印组合的十六进制: f"{high:02X}{low:02X}"
    except ValueError as e:
        print(e)
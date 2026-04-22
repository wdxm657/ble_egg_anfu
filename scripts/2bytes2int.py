def u16_from_bytes(low_byte_hex, high_byte_hex, endian="big"):
    """
    将两个十六进制字符串（低位和高位）转换为16位无符号整数。

    参数:
    low_byte_hex (str): 低位字节的十六进制表示 (例如 'A0')
    high_byte_hex (str): 高位字节的十六进制表示 (例如 'FF')
    endian (str): 字节序，'big' 表示大端序（高位在前），'little' 表示小端序（低位在前）。

    返回:
    int: 转换后的16位无符号整数。
    """
    # 将十六进制字符串转换为整数
    low_byte = int(low_byte_hex, 16)
    high_byte = int(high_byte_hex, 16)

    # 确保输入是有效的单字节 (0-255)
    if not (0 <= low_byte <= 255) or not (0 <= high_byte <= 255):
        raise ValueError("输入字节必须在 0x00 到 0xFF 之间")

    if endian == "big":
        # 大端序: 高位字节 * 256 + 低位字节
        value = (high_byte << 8) | low_byte
        print(f"大端序组合: 0x{high_byte:02X}{low_byte:02X} = {value}")
    elif endian == "little":
        # 小端序: 低位字节 * 256 + 高位字节
        value = (low_byte << 8) | high_byte
        print(f"小端序组合: 0x{low_byte:02X}{high_byte:02X} = {value}")
    else:
        raise ValueError("endian 参数必须是 'big' 或 'little'")

    return value


# --- 使用示例 ---
low = input("请输入低位: ")
high = input("请输入高位: ")

print("输入数据:")
print(f"低位: {low}")
print(f"高位: {high}")
print("-" * 30)

# 大端序转换 (通常所说的 "高位在前")
big_endian_value = u16_from_bytes(low, high, endian="big")

print("\n结论:")
print(f"整数是: {big_endian_value}")

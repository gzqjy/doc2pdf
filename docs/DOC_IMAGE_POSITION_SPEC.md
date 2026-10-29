# Microsoft Word .doc 二进制格式图片位置与尺寸计算机制

在 Microsoft Word `.doc`（二进制复合文档格式，MS-DOC / OLE2 Compound File）中，图片位置确定遵循以下底层机制：

---

## 1. 两种图片形态与正文占位符 (Two Forms & Character Tokens)

`.doc` 正文流通过 Piece Table 还原后，图片在字符流中以特殊单字符占位：

| 图片类型 | 正文占位字符 | 排版规则 |
| :--- | :---: | :--- |
| **行内嵌入式 (Inline Picture)** | `0x01` (`\x01`) | 当作“单字/行内块”参与段落折行与流动 |
| **浮动式 (Floating / Shape Object)** | `0x08` (`\x08`) | 脱离文档流，由坐标锚点决定绝对位置 |

---

## 2. 行内图片定位链路 (Inline 0x01 Resolution)

### 2.1 正文流标记
解析 Piece Table（`fcClx`）遍历正文字符流时，读取到特殊字符 `0x01`。

### 2.2 字符格式 (CHPX) 寻址
1. 读取 FIB 偏移 `0x00FA` 的 `PlcfbteChpx`，定位该 `0x01` 字符对应的属性块（CHPX）。
2. 解析属性操作符 `sprmCPicLocation` (`Opcode: 0x6A03`)。
3. 提取携带的 32 位无符号整数 `fcPic`，表示图片描述符在 `Data` 流（或 `WordDocument` 流）中的起始字节偏移量。

### 2.3 读取 PICF 描述符 (Picture Descriptor)
在指定流的 `fcPic` 偏移处解析固定 68 字节的 `PICF` 结构头：
- `dxaGoal` (offset `+28`, `uint16`): 原始宽度（单位：twips，1 pt = 20 twips）
- `dyaGoal` (offset `+30`, `uint16`): 原始高度（单位：twips）
- `mx` (offset `+32`, `uint16`): 水平缩放比例（千分比，1000 = 100%）
- `my` (offset `+34`, `uint16`): 垂直缩放比例（千分比，1000 = 100%）

目标显示尺寸计算公式：
$$\text{Width (pt)} = \frac{\text{dxaGoal} \times \text{mx}}{20 \times 1000}$$
$$\text{Height (pt)} = \frac{\text{dyaGoal} \times \text{my}}{20 \times 1000}$$

### 2.4 渲染定位
排版引擎将该图片视为尺寸为 $(\text{Width}, \text{Height})$ 的行内替换元素（Inline Block），直接放置于当前光标 $(X, Y)$ 处；若行剩余宽度或页剩余高度不足，按正文折行/分页规则流转。

---

## 3. 浮动图片定位链路 (Floating 0x08 Resolution)

针对衬于文字下方、浮于文字上方及环绕型图片：

### 3.1 字符锚点
正文字符流中遇到 `0x08` 特殊字符，标记该图片锚定的段落。

### 3.2 FSPA 表 (Format Shape Anchor)
1. 读取 FIB 偏移 `0x01F2`（`fcPlcspaMom`），定位主文档的形状锚点表。
2. 每个条目包含一个 `FSPA` 结构体：
   - `spid`: OfficeArt 形状 ID。
   - `xaLeft`, `yaTop`, `xaRight`, `yaBottom`: 矩形边界（twips）。
   - `bx` / `by`: 水平与垂直相对参考点（0=相对于页边距，1=相对于页面边缘，2=相对于段落文字）。
   - `wr`: 文字环绕方式（四周环绕、紧密环绕、浮于上方等）。

### 3.3 OfficeArt / Escher 容器
结合 Table 流中的 `OfficeArtContent`（`BStoreContainer`）提取真实图片数据，按 `FSPA` 坐标绝对渲染定位。

# LotusGBA 網路連線指南（RetroArch）

[繁體中文](gba-link-play.zh-TW.md) | [English](gba-link-play.md)

LotusGBA 的 GBA 跨系統連線仍屬實驗功能，目前主要針對英文版《神奇寶貝綠寶石》實作與驗證，並非通用的 GBA 多人連線功能。此遊戲建議預設使用 **Wireless Adapter**；日後嘗試 WAN 連線時，也優先使用這個方式。**連接線（Link Cable）**則以 LAN 連線為主。

## 支援範圍

- **Wireless Adapter：**已完成一次英文版綠寶石的真實 LAN 交換，雙方各交換一隻寶可夢（合計兩隻）；兩份存檔離線重開後都確認了交易結果。對戰尚未驗證。
- **連接線（Link Cable）：**英文版綠寶石以 LAN 使用為主。中繼連線的效能與穩定性尚未確認，不保證可可靠遊玩。
- **其他版本：**核心可辨識日文版綠寶石的 Wireless Adapter，但遊戲連線尚未驗證。日文版綠寶石與 Ruby／Sapphire 的連接線模式，目前不支援遠端連線；其他遊戲或語言版本尚未驗證。

以上僅代表列出的測試範圍，不保證其他遊戲、網路或連線活動也能正常運作。

**存檔與安全：**LotusGBA 不會傳送整份存檔。交換時，遊戲會傳送本次交易所需的寶可夢資料，並由兩端各自更新自己的存檔。使用未修改的 ROM 仍不能排除核心或網路連線風險；請只與信任的玩家連線，並在交換前備份存檔。

## 連線前

1. 雙方使用相同遊戲 ROM、相容的 LotusGBA 核心版本，並各自保留自己的存檔。
2. 在兩台裝置的 RetroArch 開啟 **快速選單 → 核心選項（Options）→ Link device**，選擇相同裝置。選項名稱目前顯示為英文 **Link device**；更改後關閉並重新載入遊戲內容。
3. 確認 RetroArch 使用的存檔位置。Netplay 設定可能會導向不同的存檔路徑。

## 開始連線

1. **同一區域網路（LAN）：**兩台裝置先連到同一個區域網路；一方直接開 Netplay 房，另一方連到該主機，不使用中繼伺服器。
2. **使用中繼伺服器（relay）：**開房者在 Netplay 設定啟用 **Use Relay Server** 後開房；另一方加入同一個房間即可，不必另外啟用中繼選項。目前版本的 Wireless Adapter 已完成一次 relay 交換；交易後存檔重開、重複連線穩定性及對戰尚未驗證。
3. RetroArch 顯示已連線，不代表遊戲內連線已完成。雙方還要在遊戲中進入相同的 Wireless Adapter 或連接線活動，依提示完成交換或對戰。
4. 正常結束活動並各自在遊戲中存檔，再正常關閉遊戲內容或 RetroArch。交換後分別離線重開兩份存檔，確認結果已保存。

若遊戲沒有連上，先確認雙方的 ROM、核心選項和遊戲活動一致。回報問題時請提供核心與 RetroArch 版本、連線裝置、LAN 或 relay、進行到的步驟及雙端 log；請勿附上 ROM 或存檔。

**技術資料：**[RFU 無線介面](gba-rfu-protocol.zh-TW.md) · [連接線](gba-pokemon-cable.zh-TW.md) · [RetroArch Netplay 指南](https://docs.libretro.com/guides/netplay-getting-started/)

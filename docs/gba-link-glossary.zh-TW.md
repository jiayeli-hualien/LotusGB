# GBA 連線術語中英對照

[RFU 繁體中文說明](gba-rfu-protocol.zh-TW.md) · [連接線繁體中文說明](gba-pokemon-cable.zh-TW.md)

這份表以繁體中文讀者為主。程式符號、遊戲選項和協定欄位保留英文原名，方便對照程式碼與 log。

| English term | 建議中文用語 | 簡要說明 |
|---|---|---|
| RFU (Radio Frequency Unit) | RFU 無線通訊介面 | GBA Wireless Adapter 使用的遊戲連線介面。本專案文件保留 RFU 縮寫。 |
| Wireless Adapter | Wireless Adapter（無線轉接器） | 綠寶石遊戲內的無線連線裝置選項；介面名稱保留英文，避免和核心選項名稱不一致。 |
| Link Cable | 連接線（Link Cable） | GBA 有線連線介面。繁中正文使用「連接線」；核心選項名稱仍保留英文 `Link Cable`。 |
| handshake | 交握 | 雙方交換必要訊息，以確認連線狀態或進入下一階段。本文統一使用「交握」，不用「握手」。 |
| peer | 對等端（peer） | 網路連線中與本端通訊的另一端。若說明具體網路位置，可用「端點」；一般敘述優先用「另一端」，避免和 RetroArch 房主／加入者混淆。 |
| endpoint | 端點 | 網路連線的一個端點；不要和 peer 完全互換，peer 強調兩端互相通訊的關係。 |
| guest / guest-side | 遊戲端／遊戲端的 | 指核心中執行的遊戲程式及其狀態。本文不譯成「訪客」，以免和 RetroArch 加入 Netplay 的 client 混淆。 |
| host / client | 房主／加入者 | RetroArch Netplay 的開房端與加入端；不要和 RFU 的 parent/child 混為一談。 |
| parent / child | RFU 主端／子端 | RFU 協定中的角色；不必然等於 RetroArch 的房主與加入者。 |
| SIO (Serial I/O) | SIO 串列輸入／輸出 | GBA 用來交換序列資料的硬體介面。程式符號維持 SIO。 |
| STWI (Serial Transfer Wireless Interface) | STWI 無線序列傳輸介面 | 綠寶石遊戲與 RFU 介面之間的命令與回覆格式。 |
| LLSF (Link Layer Subframe) | LLSF 連結層子影格 | RFU 封包中的資料單位，包含類型、序號、確認狀態和負載長度等欄位。 |
| NI (Numbered Information) | 編號資訊（NI） | RFU 分段傳送方式；遊戲以階段和序號追蹤傳送進度及確認。 |
| UNI (Un-Numbered Information) | 非編號資訊（UNI） | RFU 的另一種資料傳送方式；遊戲以新資料狀態管理接收內容，與 NI 的分段確認流程不同。 |
| ACK (acknowledgement) | 確認回覆（ACK） | 表示特定一層已收到或接受資料；需說明是哪一層的 ACK，不能一概當成遊戲已完成交換。 |
| checksum | 檢查碼 | 用來檢查資料是否一致的數值。網路封包檢查碼與遊戲序列傳輸檢查碼是不同項目。 |
| queue | 佇列 | 暫存依序處理的訊息或資料。 |
| backpressure | 背壓 | 接收端暫時無空間時，讓傳送端等待，避免丟資料或超出緩衝上限。 |
| credit / transport credit | 傳輸額度 | 網路傳輸允許尚未確認的資料量；它不代表遊戲已讀取或接受資料。 |
| LAN | 區域網路 | 同一區域網路內的直接連線情境。 |
| WAN | 廣域網路 | 經較遠網路或網際網路連線的情境。 |
| relay / relay server | 中繼／中繼伺服器 | 由伺服器轉送 Netplay 封包的連線方式。 |
| RTT (round-trip time) | 往返延遲 | 訊息往返一次所需時間；不能直接等同遊戲畫面卡頓時間。 |
| fixed delay | 固定延遲 | 測試中人為設定的單向或往返網路延遲；需標明設定方向與數值。 |
| save / SRAM | 存檔／SRAM | 遊戲保存進度的資料。雙方使用各自的存檔；測試後需分別離線重開驗證。 |
| profile | 連線設定檔 | 針對特定遊戲與連線方式採用的一組協定行為，不代表支援所有遊戲。 |
| synthetic test | 合成測試 | 以自製測試程式或測試資料驗證核心行為，不等於執行商業遊戲或通過實機驗證。 |
| guest FPS | 遊戲端每秒影格數 | 核心中遊戲程式實際前進的影格速度；不等於整個應用程式的畫面更新率。 |
| frame lead | 遊戲影格超前量 | 一端遊戲比另一端多前進的影格數；需說明是核心限制還是測試觀察。 |
| service tick | service tick（服務刻度） | 測試排程使用的離散時間單位，不等於毫秒，也不能直接換算網路延遲。 |
| session clock | 連線時鐘 | Netplay 啟動時交換的時鐘資訊；不是玩家裝置的系統時間。 |
| Ghost Send | Ghost Send（幽靈傳送／空載輪詢傳輸） | RFU 主端在沒有新應用資料時，仍發起無線交換的特殊傳輸；保留英文原名以對應 RFU 文件。 |

新增術語時，先在繁中主文件選定中文用語，再更新對應英文說明，避免兩份文件各自採用不同譯法。

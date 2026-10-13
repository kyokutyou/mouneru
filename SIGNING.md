# Windowsコード署名手順

コード署名は通常のローカルビルドには不要です。一般公開する正式なRelease成果物では、信頼できる認証局が発行したコード署名証明書の使用を推奨します。

## 前提

- Windows SDKに含まれる `signtool.exe`
- 有効なコード署名証明書
- 認証局が案内するRFC 3161タイムスタンプURL

証明書の秘密鍵やパスワードはリポジトリ、スクリプト、CIログへ保存しないでください。

## 署名

証明書ストアから証明書を自動選択する例です。`<TIMESTAMP_URL>` は認証局のURLへ置き換えます。

```powershell
signtool sign /a /fd SHA256 /tr <TIMESTAMP_URL> /td SHA256 .\build\Release\MouNeru.exe
```

PFXファイルを使う場合、パスワードは安全なシークレット管理から渡してください。コマンドラインへの平文入力は避けます。

## 検証

署名後、Windowsの信頼ポリシーを使って検証します。

```powershell
signtool verify /pa /all /v .\build\Release\MouNeru.exe
```

検証に成功した後でチェックサムを生成してください。署名するとEXEの内容とハッシュ値が変わるため、署名前のチェックサムは配布に使用できません。


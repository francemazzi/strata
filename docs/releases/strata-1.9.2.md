# Strata 1.9.2

Strata loads model choices from the connection you use. Codex shows the models
listed for your ChatGPT session. Claude subscription choices come from Claude
Code. OpenAI and Anthropic API connections use the chat models available to the saved
API key. Loading these lists does not send a chat message.

Open the chat model menu and choose **Manage models…** to refresh a list or paste
a model ID. The links beside the pickers open the provider's model documentation.
A model shown in the public API documentation may require a different connection
or plan.

For existing Codex profiles, the two choices that Strata 1.9.0 shipped
(`gpt-5.4` and `gpt-5.3-codex`) move to **Automatic** on the first launch. Automatic
uses the first model listed for the connected account. Other saved IDs are kept.
If Codex rejects a model, Strata refreshes the list so you can change the model
and resume the chat.

After updating, the existing chat history and provider credentials are still
available.

The Linux AppImage includes the OpenSSL provider used to verify update signatures.
On macOS, the updater preserves the signed application bundle and prevents Python
from writing bytecode into it during restart.

To move from Strata 1.9.0 on macOS or Linux, install this release from the DMG or
replace the old AppImage with the new one. The updater shipped in 1.9.0 can fail
to verify or install the signed update on those platforms. Your Strata profile
and projects are stored separately from the application.

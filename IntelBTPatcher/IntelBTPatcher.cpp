diff --git a/IntelBTPatcher/IntelBTPatcher.cpp b/IntelBTPatcher/IntelBTPatcher.cpp
index d2e4f72..521af9e 100644
--- a/IntelBTPatcher/IntelBTPatcher.cpp
+++ b/IntelBTPatcher/IntelBTPatcher.cpp
@@ -201,6 +201,110 @@ IOBufferMemoryDescriptor *writeHCIDescriptor = nullptr;
 #define HCI_OP_LE_READ_REMOTE_FEATURES  0x2016
 
 int remoteReadSCount = 0;
+
+#pragma mark - Disable sniff mode in link policy (AX210 A2DP start fix)
+// When an A2DP stream starts on a link that is in sniff mode, macOS sends
+// HCI_Exit_Sniff_Mode. Intel firmware sometimes answers "Command Disallowed",
+// the link stays in sniff while full-rate A2DP is pushed, the headset pauses
+// after ~3 s and the link drops with a supervision timeout ~2.5 s later.
+// Clearing the sniff bit in Write (Default) Link Policy Settings keeps ACL
+// links in active mode so Exit Sniff is never needed.
+// Boot-arg -ibtallowsniff restores the original behaviour.
+
+#define HCI_OP_WRITE_LINK_POLICY          0x080D
+#define HCI_OP_WRITE_DEFAULT_LINK_POLICY  0x080F
+#define HCI_LINK_POLICY_SNIFF             0x0004
+
+static int sniffPatchState = -1; // -1 unknown, 0 off, 1 on
+
+static bool sniffPatchEnabled()
+{
+    if (sniffPatchState < 0)
+        sniffPatchState = checkKernelArgument("-ibtallowsniff") ? 0 : 1;
+    return sniffPatchState == 1;
+}
+
+// Returns true if the HCI command in buf was modified.
+static bool stripSniffFromLinkPolicy(uint8_t *buf, uint32_t len)
+{
+    if (buf == nullptr || len < sizeof(HciCommandHdr))
+        return false;
+    HciCommandHdr *hdr = (HciCommandHdr *)buf;
+    uint32_t off;
+    if (hdr->opcode == HCI_OP_WRITE_LINK_POLICY && hdr->len >= 4 && len >= 7)
+        off = 3 + 2;  // skip connection handle
+    else if (hdr->opcode == HCI_OP_WRITE_DEFAULT_LINK_POLICY && hdr->len >= 2 && len >= 5)
+        off = 3;
+    else
+        return false;
+    uint16_t policy = (uint16_t)(buf[off] | (buf[off + 1] << 8));
+    if (!(policy & HCI_LINK_POLICY_SNIFF))
+        return false;
+    uint16_t newPolicy = policy & ~HCI_LINK_POLICY_SNIFF;
+    buf[off] = (uint8_t)(newPolicy & 0xFF);
+    buf[off + 1] = (uint8_t)(newPolicy >> 8);
+    SYSLOG(DRV_NAME, "[PATCH] opcode 0x%04x link policy 0x%04x -> 0x%04x (sniff disabled)", hdr->opcode, policy, newPolicy);
+    return true;
+}
+
+typedef struct {
+    IOUSBHostCompletion completion;     // passed down instead of the caller's
+    void *callerOwner;
+    IOUSBHostCompletionAction callerAction;
+    IOBufferMemoryDescriptor *descriptor;
+} ModifiedCommandData;
+
+static void modifiedCommandCompletion(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred)
+{
+    ModifiedCommandData *mc = (ModifiedCommandData *)owner;
+    IOUSBHostCompletionAction action = mc->callerAction;
+    void *callerOwner = mc->callerOwner;
+    mc->descriptor->complete(kIODirectionOut);
+    mc->descriptor->release();
+    IODelete(mc, ModifiedCommandData, 1);
+    if (action)
+        action(callerOwner, parameter, status, bytesTransferred);
+}
+
+static IOReturn sendModifiedCommand(void *that, IOService *provider, StandardUSB::DeviceRequest &request, uint8_t *buf, uint32_t len, unsigned int &length, IOUSBHostCompletion *completion, unsigned int timeout, bool &sent)
+{
+    sent = false;
+    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withBytes(buf, len, kIODirectionOut);
+    if (md == nullptr)
+        return kIOReturnNoMemory;
+    if (md->prepare(kIODirectionOut) != kIOReturnSuccess) {
+        md->release();
+        return kIOReturnNoMemory;
+    }
+    if (completion == nullptr) {
+        IOReturn ret = FunctionCast(CIntelBTPatcher::newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, request, nullptr, md, length, nullptr, timeout);
+        md->complete(kIODirectionOut);
+        md->release();
+        sent = true;
+        return ret;
+    }
+    ModifiedCommandData *mc = IONew(ModifiedCommandData, 1);
+    if (mc == nullptr) {
+        md->complete(kIODirectionOut);
+        md->release();
+        return kIOReturnNoMemory;
+    }
+    mc->callerOwner = completion->owner;
+    mc->callerAction = completion->action;
+    mc->descriptor = md;
+    mc->completion.owner = mc;
+    mc->completion.action = modifiedCommandCompletion;
+    mc->completion.parameter = completion->parameter;
+    IOReturn ret = FunctionCast(CIntelBTPatcher::newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, request, nullptr, md, length, &mc->completion, timeout);
+    if (ret != kIOReturnSuccess) {
+        md->complete(kIODirectionOut);
+        md->release();
+        IODelete(mc, ModifiedCommandData, 1);
+    }
+    sent = true;
+    return ret;
+}
+
 IOReturn CIntelBTPatcher::newHostDeviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor, unsigned int &length, IOUSBHostCompletion *completion, unsigned int timeout)
 {
     HciCommandHdr *hdr = nullptr;
@@ -265,6 +369,17 @@ IOReturn CIntelBTPatcher::newHostDeviceRequest(void *that, IOService *provider,
         }
 #endif
     }
+    if (sniffPatchEnabled()) {
+        if (data != nullptr) {
+            stripSniffFromLinkPolicy((uint8_t *)data, request.wLength);
+        } else if (descriptor != nullptr && hdrLen > 0 && stripSniffFromLinkPolicy((uint8_t *)hciBuf, hdrLen)) {
+            bool sent = false;
+            IOReturn ret = sendModifiedCommand(that, provider, request, (uint8_t *)hciBuf, hdrLen, length, completion, timeout, sent);
+            if (sent)
+                return ret;
+            SYSLOG(DRV_NAME, "[PATCH] could not resend modified link policy (%d), sending original", ret);
+        }
+    }
     return FunctionCast(newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, request, data, descriptor, length, completion, timeout);
 }

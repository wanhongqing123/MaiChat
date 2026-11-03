// 群聊验收的隔离身份和假数据，不进入 Release。
#if DEBUG && targetEnvironment(simulator)
import Foundation
import MaiChatCore

enum GroupChatUITestData {
    static let ownerID = "sim-group-owner"
    static let nickname = "模拟器测试账号"
    static let switchedOwnerID = "sim-group-owner-switched"
    static let switchedNickname = "切换后的测试账号"
    static var contacts: [RemoteIMContact] {
        [RemoteIMContact(userID: "sim-friend-a", displayName: "测试好友 A", relation: .friend),
         RemoteIMContact(userID: "sim-friend-b", displayName: "测试好友 B", relation: .friend)]
    }
}
#endif

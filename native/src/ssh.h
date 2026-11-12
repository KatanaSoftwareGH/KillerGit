#pragma once
#include "util.h"

// SSH key management through Windows' built-in OpenSSH (C:\Windows\System32\OpenSSH).
namespace ssh {

struct Key {
  std::string name, path, pubPath, type, comment, fingerprint, publicKey;
  bool hasPrivate = false;
};

std::string exe(const std::string &name);   // full path to ssh / ssh-keygen / ssh-add
std::string sshDir();
bool available();
std::vector<Key> list(bool withFingerprints = true);
Strings privateKeys();                       // private keys that have a .pub sibling
std::vector<Key> scanFolder(const std::string &dir);  // any key-looking files in a folder

std::string generate(const std::string &name, const std::string &type, const std::string &comment, const std::string &passphrase);
void setComment(const std::string &keyPath, const std::string &comment, const std::string &passphrase = "");
void changePassphrase(const std::string &keyPath, const std::string &oldP, const std::string &newP);
std::string rename(const std::string &keyPath, const std::string &newName);
void remove(const std::string &keyPath);
std::string import(const std::string &name, const std::string &privateKey);
std::string addToAgent(const std::string &keyPath);
std::string agentList();
std::string test(const std::string &host, const std::string &keyPath);

}  // namespace ssh

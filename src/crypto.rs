extern crate sodiumoxide;

use sodiumoxide::crypto::{auth, pwhash};
use sodiumoxide::utils;

use types::User;
use database::Connection;
use database;

pub const KEYBYTES: usize = 32;
pub const HASHBYTES: usize = 32;
pub const DERIVBYTES: usize = KEYBYTES + HASHBYTES;

pub type Key = [u8; KEYBYTES];
pub type Hash = [u8; HASHBYTES];

pub use sodiumoxide::init;

fn derive_key(key: &mut Key, hash: &mut Hash,
              pw: &str, salt: &[u8; pwhash::SALTBYTES]) {
    let mut res = [0u8; DERIVBYTES];
    pwhash::derive_key(
        &mut res,
        pw.as_bytes(),
        &pwhash::Salt::from_slice(salt).unwrap(),
        pwhash::OPSLIMIT_INTERACTIVE,
        pwhash::MEMLIMIT_INTERACTIVE).unwrap();

    key.copy_from_slice(&res[0 .. KEYBYTES]);
    hash.copy_from_slice(&res[KEYBYTES .. DERIVBYTES]);
}

pub fn get_key(user: &User, pw: &str) -> Key {
    let mut k: Key = Default::default();
    let mut h: Hash = Default::default();

    derive_key(&mut k, &mut h, pw, &user.salt);
    if !utils::memcmp(&h, &user.hash) {
        panic!("Password incorrect");
    }

    k
}

pub fn derive_subkey(k: &Key, id: &[u8]) -> Key {
    let auth::Tag(sk) = auth::authenticate(id,
            &auth::Key::from_slice(k).unwrap());
    sk
}

pub fn create_user(pw: &str) -> (User, Key) {
    let mut u: User = Default::default();

    let pwhash::Salt(salt) = pwhash::gen_salt();
    u.salt.copy_from_slice(&salt);

    let mut k: Key = Default::default();
    derive_key(&mut k, &mut u.hash, pw, &salt);

    (u, k)
}

pub fn compute_file_sig(key: &Key, conn: &Connection) -> [u8; auth::TAGBYTES] {
    let skey = derive_subkey(key, "VERIFY".as_bytes());

    let mut state = auth::State::init(&skey);
    for passw in database::get_passwords(conn) {
        state.update(&passw.id);
        state.update(passw.name.as_slice());
        state.update(passw.password.as_slice());
    }

    let auth::Tag(digest) = state.finalize();

    digest
}

pub fn verify_file(user: &User, key: &Key, conn: &Connection) -> bool {
    sodiumoxide::utils::memcmp(&user.sig, &compute_file_sig(key, conn))
}

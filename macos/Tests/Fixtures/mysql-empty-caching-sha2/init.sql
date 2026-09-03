CREATE DATABASE IF NOT EXISTS gridex_empty_auth;
CREATE USER IF NOT EXISTS 'gridex_empty'@'%'
  IDENTIFIED WITH caching_sha2_password BY '';
GRANT ALL PRIVILEGES ON gridex_empty_auth.* TO 'gridex_empty'@'%';
CREATE USER IF NOT EXISTS 'gridex_nonempty'@'%'
  IDENTIFIED WITH caching_sha2_password BY 'gridex-secret';
GRANT ALL PRIVILEGES ON gridex_empty_auth.* TO 'gridex_nonempty'@'%';
FLUSH PRIVILEGES;

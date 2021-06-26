#ifndef _PASSWORD_H_
#define _PASSWORD_H_


/**
 * @brief initialize password support
 * 
 * @return err if settings storage cannot be read
 */
int init_passwd(void);

/**
 * @brief check password
 * Confirm hash of password matches stored hash.
 * 
 * @return err if hash does not match.
 */
int check_passwd(const char *passwd);

/**
 * @brief is valid password
 * Test whether user-provided password meets minimum quality standards.
 * 
 * @return -EINVAL if length is too short or too long.
 */
int is_valid_passwd(const char *passwd);

/**
 * @brief set password
 * Attempt to hash and store the provided password.
 * 
 * @return err if cannot be stored to settings.
 */
int set_passwd(const char *passwd);

#endif

